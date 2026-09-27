/*
 * time_mgr.c —— SNTP 时间同步的实现
 *
 * 实现刻意做得很薄：把 esp_netif 提供的 SNTP 封装成"不阻塞 init +
 * 可选 wait + 随时可查"三件事，与 wifi_mgr 的形状保持一致。
 *
 * 为什么用 esp_netif_sntp 而不是 lwip 的 sntp_* 裸接口？
 *
 *   lwIP 的 sntp_* 是纯协议实现：只负责发 UDP 包、收包、调
 *   settimeofday()。DNS 解析、网络接口选择、IP 事件时机这些都要
 *   调用方自己安排，还得自己判断"现在有没有网"。
 *
 *   esp_netif_sntp 是 ESP-IDF 在它外面包的一层：帮我们处理了
 *   与默认网络接口和事件循环的衔接，并且**自带一个 lwIP 定时器上下文**——
 *   同步是它自己在后台做的，不需要我们建任务或定时器。
 *
 *   所以本组件**没有自己的任务**。文档 §6 里画的 TimeSyncTask 在这里
 *   是多余的：为一个"每 15 秒发一个 UDP 包"的动作专门开一个任务，
 *   多占 2~3KB 栈和一整套任务切换开销，换不来任何东西。
 *   （判断依据是"这件事需不需要一个独立的时间线"——SNTP 的重试节奏
 *     由 lwIP 内部定时器驱动，与我们的任务无关，所以不需要。）
 *
 * 关于"同步成功的标志位"
 *
 *   用一个 event group 的 bit0，与 wifi_mgr 的做法一致。为什么不用
 *   一个 volatile bool？因为 wait_synced() 需要"能等"，而"能等"必须
 *   靠内核对象；如果标志位和等待对象是两个东西，就可能出现
 *   "bit 已经置上、但等待者还在睡"的不一致。用同一个事件组，
 *   is_synced() 读的和 wait_synced() 等的是同一个来源，不可能矛盾。
 */
#include "time_mgr.h"

#include <sys/time.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"

static const char *TAG = "time_mgr";

/*
 * 事件组里"已完成过至少一次同步"那一位（bit0）。
 * 直接写字面量的理由同 wifi_mgr.c：BIT0 定义在 esp_bit_defs.h，
 * 这条包含链上没有头文件会把它带进来。
 */
#define TIME_MGR_SYNCED_BIT ((EventBits_t)0x01)

static bool s_initialized = false;

/*
 * 同步状态的唯一来源。is_synced() 读它，wait_synced() 等它。
 * 见文件头"关于同步成功的标志位"。
 */
static EventGroupHandle_t s_event_group = NULL;

/*
 * SNTP 同步回调。
 *
 * 上下文：由 lwIP 的 SNTP 定时器调用，运行在 tcpip 线程里（**任务上下文，
 * 不是中断**），所以可以直接调 xEventGroupSetBits()。如果哪天这里变成
 * 从 ISR 调用，必须换成 xEventGroupSetBitsFromISR() 并把返回值交给
 * portYIELD_FROM_ISR——两者不能混用。
 *
 * 这个回调**每次同步都会调**（首次同步 + 之后每小时一次的重同步），
 * 所以置位操作必须是幂等的——xEventGroupSetBits 天然幂等，重复置同一位
 * 没有副作用。重同步会把系统时钟重新校准（lwIP 默认每
 * CONFIG_LWIP_SNTP_UPDATE_DELAY 毫秒同步一次，本工程是 1 小时），
 * 顺便抵消 RTC 的累积漂移。
 */
static void on_time_synced(struct timeval *tv)
{
    if (s_event_group != NULL) {
        xEventGroupSetBits(s_event_group, TIME_MGR_SYNCED_BIT);
    }

    /*
     * 打一条日志，而且**只在第一次**有意义地打。
     * 这是启动日志里唯一能确认"NTP 真的通了"的地方——没有它，
     * 时间同步失败的表现是完全静默的（报文里 ts 是个小数字，
     * 不仔细看根本发现不了）。重同步时也打，用于观察时钟漂移量。
     */
    ESP_LOGI(TAG, "时间已同步：Unix %lld 秒（此后报文时间戳为 Unix 毫秒）",
             (long long)tv->tv_sec);
}

esp_err_t time_mgr_init(const time_mgr_config_t *config)
{
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * 空服务器 = 不做时间同步，这是受支持的用法（见头文件约定一）。
     * 与 wifi_mgr 的空 SSID 同一模式：**在碰任何 IDF 接口之前**就返回，
     * 免得留下"初始化过一半"的状态。
     */
    if (config == NULL || config->server == NULL || config->server[0] == '\0') {
        ESP_LOGW(TAG, "未配置 NTP 服务器，本次不做时间同步；"
                      "报文时间戳为开机毫秒数（clock 字段会标成 boot）");
        return ESP_ERR_INVALID_ARG;
    }

    /* 事件组必须在启动 SNTP **之前**建好：同步回调可能在任意时刻触发 */
    s_event_group = xEventGroupCreate();
    if (s_event_group == NULL) {
        ESP_LOGE(TAG, "创建事件组失败");
        return ESP_ERR_NO_MEM;
    }

    const esp_sntp_config_t cfg = {
        /*
         * smooth_sync = false：用 settimeofday() 一次性跳变到正确时间。
         *
         * true 的话 lwIP 会以每 200ms 最多 5ms 的步进慢慢把时钟拨过去，
         * 好处是时间单调不回退（对日志时间戳友好），代价是要几分钟才追上。
         * 这里选跳变，因为设备刚开机、还没有任何依赖单调性的数据。
         */
        .smooth_sync = false,

        /*
         * server_from_dhcp = false：不从 DHCP 选项里取 NTP 服务器。
         * 家用路由器极少下发这项，依赖它等于把"能不能同步"交给运气。
         * 服务器由 Kconfig 明确指定，行为可预期。
         */
        .server_from_dhcp = false,

        /*
         * wait_for_sync = true：让 esp_netif 建一个内部信号量，
         * 这样 esp_netif_sntp_sync_wait() 才有东西可等。
         * 本组件自己用事件组等，但保持这个选项为 true 不会有害，
         * 且将来若要直接用那个接口也不必改。
         */
        .wait_for_sync = true,

        /* start = true：init 后立刻开始同步，不等调用方再调一次 start */
        .start = true,

        /* 同步回调，用来置位 + 打日志 */
        .sync_cb = on_time_synced,

        /*
         * renew_servers_after_new_IP = false：拿到 IP 后不刷新服务器列表。
         * 那个选项是给 server_from_dhcp 用的（DHCP 提供的服务器会清掉
         * 预配置的），我们不用 DHCP 取服务器，所以关掉。
         */
        .renew_servers_after_new_IP = false,
        .ip_event_to_renew = IP_EVENT_STA_GOT_IP,
        .index_of_first_server = 0,

        /* 只有一台服务器，上限来自 CONFIG_LWIP_SNTP_MAX_SERVERS=1 */
        .num_of_servers = 1,
        .servers = { config->server },
    };

    const esp_err_t ret = esp_netif_sntp_init(&cfg);
    if (ret != ESP_OK) {
        /*
         * 失败要回滚：把事件组删掉，让 s_event_group 回到 NULL。
         * 不回滚的话，调用方之后调 is_synced()/wait_synced() 会看到
         * 一个"永远等不到结果"的事件组，比明确失败更难排查。
         */
        vEventGroupDelete(s_event_group);
        s_event_group = NULL;
        ESP_LOGE(TAG, "启动 SNTP 失败: %s", esp_err_to_name(ret));
        return ret;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "SNTP 已启动，服务器 %s（首次同步前时间戳为开机毫秒）",
             config->server);
    return ESP_OK;
}

esp_err_t time_mgr_wait_synced(uint32_t timeout_ms)
{
    if (s_event_group == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * xClearOnExit = pdFALSE：拿到之后不清零，这样 is_synced() 之后
     * 还能读到"已同步"。粘滞语义，理由见头文件。
     */
    const EventBits_t bits = xEventGroupWaitBits(s_event_group, TIME_MGR_SYNCED_BIT,
                                                pdFALSE, pdFALSE,
                                                pdMS_TO_TICKS(timeout_ms));

    return ((bits & TIME_MGR_SYNCED_BIT) != 0) ? ESP_OK : ESP_ERR_TIMEOUT;
}

bool time_mgr_is_synced(void)
{
    /*
     * 未初始化（或初始化失败回滚过）时没有事件组可读，直接回答"没同步"。
     * 与 wifi_mgr_is_connected() 一样，这里不打日志——本函数会被
     * 上报任务每轮调用，打日志只会淹没真正的问题。
     */
    if (s_event_group == NULL) {
        return false;
    }

    return (xEventGroupGetBits(s_event_group) & TIME_MGR_SYNCED_BIT) != 0;
}

int64_t time_mgr_now_unix_ms(void)
{
    if (!time_mgr_is_synced()) {
        return -1;
    }

    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

int64_t time_mgr_boot_ms_to_unix_ms(int64_t boot_ms)
{
    if (!time_mgr_is_synced()) {
        return boot_ms;
    }

    /*
     * 偏差每调用一次算一次，而不是同步时算一次存起来。
     *
     * 为什么？因为 lwIP 会周期性重同步（默认 1 小时），每次重同步
     * 都可能把时钟微调一下（校正 RTC 漂移）。现算的话，
     * 偏差永远反映"此刻"的真实关系；存起来的话，重同步之后就过期了，
     * 而且过期这件事不会有任何提示。
     *
     * 代价是每次调用多两次取时间（一次 gettimeofday，一次
     * esp_timer_get_time），都是几条指令，在 2 秒一次的上报节奏下
     * 完全无所谓。
     *
     * 注意 esp_timer_get_time() 的单位是微秒，要除 1000 换成毫秒，
     * 与 sensor_task 填 timestamp_ms 时的换算保持一致。
     */
    const int64_t now_unix_ms = time_mgr_now_unix_ms();
    const int64_t now_boot_ms = esp_timer_get_time() / 1000;

    return boot_ms + (now_unix_ms - now_boot_ms);
}
