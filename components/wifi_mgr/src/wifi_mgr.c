/*
 * wifi_mgr.c —— WiFi Station 的实现
 *
 * 设计要点（对应 wifi_mgr.h 里的三条约定，这里说"怎么实现"）：
 *
 * 1. 状态只有一份 —— 事件组里的 WIFI_MGR_CONNECTED_BIT
 *
 *    连接状态本来可以存成一个 static bool，但那样就有了两份真相：
 *    bool（给 is_connected 用）和事件组标志位（给 wait_connected 用）。
 *    两份真相迟早会不一致——比如某条路径忘了更新 bool，于是
 *    is_connected() 说"连上了"而 wait_connected() 还在死等。
 *    所以这里只保留事件组，is_connected() 也去读它。
 *    这也是 FreeRTOS 事件组的典型用法：一个位表示一个状态，
 *    多个任务可以同时等它、随时查它。
 *
 * 2. 回调运行在事件循环任务里，不是中断上下文
 *
 *    esp_event 的默认事件循环有自己的任务，回调在**任务上下文**里执行，
 *    所以可以直接调 esp_wifi_connect()、xEventGroupSetBits() 这些
 *    可能阻塞的接口。这和 button 组件里 ISR 必须用 FromISR 后缀版本
 *    形成对比——写回调前第一件事就是判断"我在什么上下文里"。
 *
 * 3. 断线走 ESP_LOGW 而不是 ESP_LOGE
 *
 *    断线属于"辅助功能失败"：设备的本地功能（LED、按键、RGB、
 *    温湿度采集）一个都不受影响，只是不能上报数据。
 *    把它打成 ERROR 会让人误以为系统坏了，还会在串口里淹没真正的问题。
 *    同样的判断在 components/app/src/button_task.c 里出现过一次
 *    （NVS 保存失败用 LOGW），两处遵循同一个原则：
 *    **区分"功能失败"和"辅助功能失败"**。
 *
 * 4. 先校验、后动手
 *
 *    wifi_mgr_init() 把**所有**参数校验放在最前面，一行 ESP-IDF 接口都
 *    还没碰。原因见 init 函数里的注释：空 SSID 是"不联网"这条受支持路径
 *    的信号，必须在网络接口、事件循环、WiFi 驱动都没被初始化过的时候
 *    就返回，否则这条路径会留下一堆半初始化的资源。
 *
 * 5. 连接从 WIFI_EVENT_STA_START 事件发起，不在 esp_wifi_start() 之后直接调
 *
 *    见 on_wifi_event() 里的说明。
 *
 * 本组件**不调用 nvs_flash_init()**：NVS 由 settings 组件在 app_start()
 * 最开始处挂载好（详见 CMakeLists.txt 里的说明），重复初始化是重复职责。
 */
#include "wifi_mgr.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"

static const char *TAG = "wifi_mgr";

/*
 * 日志节流参数。注意节流的是**打印**，不是**重连**——
 * 重连永远不停，见 on_wifi_event() 里的说明。
 *
 *   前 WIFI_MGR_LOG_ALWAYS_RETRIES 次断线每次都打，方便排查"为什么连不上"
 *   （开机阶段的前几次最有诊断价值：reason 码直接指出是密码错还是找不到 AP）；
 *   之后每 WIFI_MGR_LOG_EVERY_N_RETRIES 次打一条，带累计次数，
 *   这样路由器长时间不可达也不会把串口刷满。
 */
#define WIFI_MGR_LOG_ALWAYS_RETRIES     3
#define WIFI_MGR_LOG_EVERY_N_RETRIES    10

/*
 * 事件组里"已连接并拿到 IP"那一位（bit0）。只用一位，其余留给将来扩展。
 *
 * 这里直接写字面量，而不是用 IDF 代码里常见的 BIT0：
 * BIT0 定义在 esp_bit_defs.h（esp_common 组件）里，而本文件 include 的
 * esp_wifi / esp_netif / esp_event / freertos 这一串头文件**都不会**把它
 * 带进来（这条包含链上没有任何一个头文件 include 它），
 * 直接写 BIT0 会报"未声明的标识符"。为一个常量再专门引一个头文件不划算。
 */
#define WIFI_MGR_CONNECTED_BIT ((EventBits_t)0x01)

/* SSID / 密码的长度上限，来自 esp_wifi_types_generic.h 的定长数组尺寸 */
#define WIFI_MGR_SSID_MAX_LEN       32
#define WIFI_MGR_PASSWORD_MAX_LEN   64

static bool s_initialized = false;

/*
 * 连接状态的唯一来源。
 * 用事件组而不是额外的 bool：wait_connected() 等的是它，is_connected()
 * 读的也是它，两者不可能不一致（详见文件头说明 1）。
 */
static EventGroupHandle_t s_event_group = NULL;

/* 默认 STA 网络接口。留下句柄是为了初始化失败回滚时能销毁它 */
static esp_netif_t *s_netif = NULL;

/*
 * 事件回调的实例句柄。
 * esp_event_handler_instance_register() 会把"这一次注册"的句柄写回来，
 * 有了它才能精确地反注册本组件注册的那两个回调，
 * 而不会误伤别的组件注册的处理函数（比如 esp_netif 自己注册的那批）。
 */
static esp_event_handler_instance_t s_wifi_handler = NULL;
static esp_event_handler_instance_t s_ip_handler = NULL;

/* 本次断线以来已经重连的次数。拿到 IP 时清零 */
static uint32_t s_retry_count = 0;

/* wifi_mgr_init() 存下的默认等待超时，供 wait_connected(0) 使用 */
static uint32_t s_connect_timeout_ms = 0;

/*
 * WIFI_EVENT 回调。
 *
 * 注册时用的是 ESP_EVENT_ANY_ID，所以驱动侧所有事件都会进到这里；
 * 本组件只关心 STA_START 和 STA_DISCONNECTED 两个，其余忽略。
 *
 * 上下文：在 esp_event 默认事件循环的任务里执行（**不是中断**），
 * 所以可以直接调可能阻塞的 esp_wifi_connect()。
 */
static void on_wifi_event(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    /* 注册时没传自定义参数。显式 void 掉，避免"未使用参数"的告警 */
    (void)arg;
    (void)event_base;

    if (event_id == WIFI_EVENT_STA_START) {
        /*
         * 驱动真正就绪了，这才是发起连接的时机。
         *
         * 为什么不放在 esp_wifi_start() 之后直接调？
         *   esp_wifi_start() 只是"请求启动"，驱动内部的协议栈任务和状态机
         *   是异步建立起来的，完成后才会发出本事件。在它之前调
         *   esp_wifi_connect()，会撞上"驱动还没准备好"的竞态，
         *   典型症状是拿到 ESP_ERR_WIFI_NOT_STARTED，或者时序错乱导致
         *   连接莫名其妙地失败。官方 station 示例就是在本事件里连的，
         *   这里保持一致。
         */
        ESP_LOGI(TAG, "WiFi 驱动已启动，发起连接");
        esp_err_t ret = esp_wifi_connect();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "发起连接失败: %s", esp_err_to_name(ret));
        }
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event =
            (const wifi_event_sta_disconnected_t *)event_data;

        /*
         * 断线属于"辅助功能失败"：本地功能一个都不受影响，只是不能联网。
         * 所以这里
         *   - 日志级别用 W 而不是 E（用 E 会让人误以为系统坏了）
         *   - 立刻清掉连接标志位，让 is_connected() 反映真实状态
         *   - 绝不 abort、绝不 ESP_ERROR_CHECK
         * 这个判断和 components/app/src/button_task.c 里"NVS 保存失败用
         * LOGW"是同一个原则。
         */
        xEventGroupClearBits(s_event_group, WIFI_MGR_CONNECTED_BIT);

        /*
         * 先自增再判断，让日志里的次数从 1 开始数，读起来直观。
         * reason 是 uint8_t、rssi 是 int8_t，按本工程习惯显式转换后
         * 分别用 %u / %d 打印。
         */
        s_retry_count++;

        /*
         * 只节流日志，不节流重连：s_retry_count 到多少都照打不误，
         * 但打印频率降下来——路由器长时间不可达时，同一条日志会把串口里
         * 真正有用的信息淹没，而"不打印"和"不重连"是两件完全不同的事。
         */
        const bool should_log = (s_retry_count <= WIFI_MGR_LOG_ALWAYS_RETRIES) ||
                                (s_retry_count % WIFI_MGR_LOG_EVERY_N_RETRIES == 0);

        if (should_log) {
            /* 每次重连只占一行日志，方便在串口输出里过滤 */
            ESP_LOGW(TAG, "连接断开（reason=%u, rssi=%d），累计第 %u 次重连",
                     (unsigned)event->reason, (int)event->rssi,
                     (unsigned)s_retry_count);
        }

        /*
         * ★ 无条件重连，**永不放弃**。
         *
         * 早期版本写的是"连续失败 5 次就停手"，看起来能防刷屏，实际是个坑：
         * 路由器开机时不在、路由重启、AP 断电，都会让设备在启动阶段就用完
         * 重试预算，之后**永久**失去联网能力，只能靠手动复位恢复。
         * 对一个常驻的传感节点来说这是很糟的失败模式。
         *
         * 真正要解决的问题是"日志刷屏"，那就只解决它（上面的节流），
         * 而不是顺手把一个不可恢复的状态引入系统。
         *
         * 调用本身失败时**不跟着节流**地报出来，理由有两条：
         *   1. 它的频率被 DISCONNECTED 事件本身的节奏限制着（驱动每次尝试
         *      连接都要等到超时才失败），不会像固定周期重试那样高频刷屏；
         *   2. 它一旦出现，往往意味着"重连链断了"——connect 请求都发不出去
         *      （比如驱动没启动），就不会再有新的断开事件。这一行之后串口
         *      会安静下来，藏起来最要命：会让人误以为"还在重试"。
         */
        esp_err_t ret = esp_wifi_connect();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "重连请求失败: %s（可能不会再收到断开事件）",
                     esp_err_to_name(ret));
        }
    }
}

/*
 * IP_EVENT 回调。
 *
 * 只注册了 IP_EVENT_STA_GOT_IP 一个事件，所以进来就意味着
 * "DHCP 成功、拿到 IP、网络真的可以用了"。
 *
 * 为什么"已连接"的判定放在这里而不是 WIFI_EVENT_STA_CONNECTED？
 *   和 AP 建立链路层连接（CONNECTED）之后还要走 DHCP，此时**还不能
 *   收发数据**。只有 GOT_IP 才代表可用，对上层来说这才叫"连上了"。
 */
static void on_ip_event(void *arg, esp_event_base_t event_base,
                        int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;

    /* 防御性判断：将来若注册了别的 IP 事件，这里不会误设标志位 */
    if (event_id != IP_EVENT_STA_GOT_IP) {
        return;
    }

    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;

    /*
     * 重连计数必须清零。不清的话计数只增不减，日志节流会一路降到
     * 每 10 次才打一条，"前几次断线全都打出来"的诊断价值也就没了。
     */
    s_retry_count = 0;
    xEventGroupSetBits(s_event_group, WIFI_MGR_CONNECTED_BIT);

    /*
     * IPSTR / IP2STR 定义在 esp_netif 的公开头文件 esp_netif_ip_addr.h 里，
     * 所以这里不需要直接 include lwIP 的任何头文件。
     */
    ESP_LOGI(TAG, "已连接，IP: " IPSTR, IP2STR(&event->ip_info.ip));
}

/*
 * 回滚已经建立起来的状态。
 *
 * 只在 wifi_mgr_init() 的失败路径上调用，所以有两个前提，写在这里免得
 * 以后有人复用它时踩坑：
 *
 *   - 此时驱动一定处于未启动状态：esp_wifi_start() 是最后一步，
 *     只有它**之前**的步骤失败才会走到这里。而 esp_wifi_deinit()
 *     不允许在 started 状态下调用。
 *
 *   - 参数用两个布尔量说明"走到哪一步了"，只回滚真正成功过的步骤。
 *     对没做过的事做清理（比如 deinit 一个从未 init 过的驱动）虽然无害，
 *     但会返回 ESP_ERR_WIFI_NOT_INIT，日志反而误导人。
 *
 * 清理动作本身失败只记 W 级日志、不改变返回值：真正的错误原因是调用方
 * 关心的那个 ret，用回滚过程中的次生错误覆盖它只会让排查更困难。
 * （app_start() 把初始化失败当致命错误处理，所以这里的回滚是"卫生措施"，
 *   不是"为了让重试能成功"。）
 */
static void unwind_partial_init(bool wifi_inited, bool netif_created)
{
    if (s_ip_handler != NULL) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                             s_ip_handler);
        s_ip_handler = NULL;
    }
    if (s_wifi_handler != NULL) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                             s_wifi_handler);
        s_wifi_handler = NULL;
    }

    if (wifi_inited) {
        esp_err_t ret = esp_wifi_deinit();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "回滚：释放 WiFi 驱动失败: %s", esp_err_to_name(ret));
        }
    }

    if (netif_created) {
        /* 这个函数对 NULL 是安全的（头文件写明是 no-operation） */
        esp_netif_destroy_default_wifi(s_netif);
    }
    s_netif = NULL;

    if (s_event_group != NULL) {
        vEventGroupDelete(s_event_group);
        s_event_group = NULL;
    }

    s_initialized = false;
    s_retry_count = 0;
    s_connect_timeout_ms = 0;
}

esp_err_t wifi_mgr_init(const wifi_mgr_config_t *config)
{
    /*
     * ★ 全部参数校验都在这上面，一行 ESP-IDF 接口都还没碰。
     *
     * 为什么顺序这么重要？
     *   本工程的 app.c 把 wifi_mgr_init() 返回的 ESP_ERR_INVALID_ARG
     *   当作**受支持的**"不联网、继续开机"信号（空 SSID 就是这条路径）。
     *   如果校验发生在 esp_netif_init() / esp_event_loop_create_default() /
     *   esp_netif_create_default_wifi_sta() / esp_wifi_init() 之后，
     *   那么"没配置 SSID"这一路会留下：已初始化的网络接口层、
     *   被占用的默认事件循环、一个没人用的 netif，以及一个 init 过却
     *   从未 start 的 WiFi 驱动——一堆半初始化状态，比"什么都没碰"
     *   难推理得多（比如上层将来想重新 init，就会撞上"事件循环已存在"）。
     *   所以校验一律前置，让这条路径**完全没有副作用**。
     */
    if (s_initialized) {
        ESP_LOGE(TAG, "已经初始化过了");
        return ESP_ERR_INVALID_STATE;
    }

    if (config == NULL) {
        ESP_LOGE(TAG, "配置非法");
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * 空 SSID 是**受支持的**离线运行方式，不是"配置错了"。
     * 所以用 W 而不是 E，并在日志里说清怎么办、以及不影响什么。
     * ESP_ERR_INVALID_ARG 是这里唯一的信号通道，调用方据此跳过联网。
     */
    if (config->ssid == NULL || config->ssid[0] == '\0') {
        ESP_LOGW(TAG, "没有配置 SSID，WiFi 不联网"
                      "（运行 idf.py menuconfig → WiFi 连接 填好后重新构建）；"
                      "设备其余功能不受影响");
        return ESP_ERR_INVALID_ARG;
    }

    const size_t ssid_len = strlen(config->ssid);
    if (ssid_len > WIFI_MGR_SSID_MAX_LEN) {
        ESP_LOGE(TAG, "SSID 长 %u 字节，超过上限 %d 字节",
                 (unsigned)ssid_len, WIFI_MGR_SSID_MAX_LEN);
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * 密码为 NULL 直接拦住：紧接着的 memcpy 会立刻崩，而"上电就崩"
     * 比"连不上"难排查得多。开放网络请传空串 ""。
     */
    if (config->password == NULL) {
        ESP_LOGE(TAG, "密码指针为 NULL；开放网络请传空串");
        return ESP_ERR_INVALID_ARG;
    }

    const size_t password_len = strlen(config->password);
    if (password_len > WIFI_MGR_PASSWORD_MAX_LEN) {
        ESP_LOGE(TAG, "密码长 %u 字节，超过上限 %d 字节",
                 (unsigned)password_len, WIFI_MGR_PASSWORD_MAX_LEN);
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * ── 校验结束，从这里开始才真正碰 ESP-IDF ──────────────────────
     */

    /*
     * 事件组先建。
     * 顺序很关键：回调里要往里设标志位，如果先注册回调再建事件组，
     * 中间这一小段里到达的事件就会去写一个 NULL 句柄——而 WiFi 事件是
     * 异步的，可能在任意时刻到达。先把"接收端"准备好，再让"发送端"开跑。
     */
    s_event_group = xEventGroupCreate();
    if (s_event_group == NULL) {
        ESP_LOGE(TAG, "创建事件组失败");
        return ESP_ERR_NO_MEM;
    }
    s_retry_count = 0;

    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "初始化网络接口层失败: %s", esp_err_to_name(ret));
        unwind_partial_init(false, false);
        return ret;
    }

    /*
     * 默认事件循环，WiFi / IP 事件都投递到它上面。
     *
     * 这里只接受 ESP_OK：如果已经有别的组件建过事件循环，本函数会返回
     * ESP_ERR_INVALID_STATE。那说明事件循环是按别人的意图配的，
     * 本组件不该假装成功——本工程 app_start() 把初始化失败当致命错误，
     * 让问题暴露在启动阶段，比让它潜伏到运行期好。
     */
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "创建默认事件循环失败: %s", esp_err_to_name(ret));
        unwind_partial_init(false, false);
        return ret;
    }

    /*
     * 创建默认 STA 网络接口。它一次做三件事：建 esp_netif 对象、
     * 把它和 WiFi 驱动绑起来、注册 netif 自己那批默认事件处理函数
     * （DHCP 客户端就在那里被启动，IP 也是那里申请到的）。
     *
     * 头文件注明这个函数内部用 assert 检查错误，也就是某些失败会直接
     * 在它内部 abort，根本不会返回 NULL。这里的 NULL 判断是兜底：
     * 能拦住的比漏出去好，但别指望它是完整的错误路径。
     */
    s_netif = esp_netif_create_default_wifi_sta();
    if (s_netif == NULL) {
        ESP_LOGE(TAG, "创建默认 STA 网络接口失败");
        unwind_partial_init(false, false);
        return ESP_ERR_NO_MEM;
    }

    /*
     * WIFI_INIT_CONFIG_DEFAULT() 是必须用的：它填进去的 magic 字段是
     * 驱动校验配置合法性的依据，手写一个空结构体会被驱动拒绝。
     */
    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&init_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "初始化 WiFi 驱动失败: %s", esp_err_to_name(ret));
        unwind_partial_init(false, true);
        return ret;
    }

    /*
     * 注册两个回调，并把实例句柄存下来。
     *
     *   WIFI_EVENT + ESP_EVENT_ANY_ID —— 驱动侧所有事件都收，
     *       本组件只处理 STA_START / STA_DISCONNECTED，其余在回调里忽略。
     *   IP_EVENT + IP_EVENT_STA_GOT_IP —— 只在拿到 IP 时收。
     *
     * 用 instance 版本而不是 esp_event_handler_register()，是为了拿到实例
     * 句柄——将来要反注册时能精确摘掉自己这一份，不会误伤别人。
     */
    ret = esp_event_handler_instance_register(WIFI_EVENT,
                                              ESP_EVENT_ANY_ID,
                                              on_wifi_event,
                                              NULL,
                                              &s_wifi_handler);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "注册 WiFi 事件回调失败: %s", esp_err_to_name(ret));
        unwind_partial_init(true, true);
        return ret;
    }

    ret = esp_event_handler_instance_register(IP_EVENT,
                                              IP_EVENT_STA_GOT_IP,
                                              on_ip_event,
                                              NULL,
                                              &s_ip_handler);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "注册 IP 事件回调失败: %s", esp_err_to_name(ret));
        unwind_partial_init(true, true);
        return ret;
    }

    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "设置 WiFi 模式为 STA 失败: %s", esp_err_to_name(ret));
        unwind_partial_init(true, true);
        return ret;
    }

    /*
     * 填 SSID / 密码。两个细节都是容易踩的坑：
     *
     * 1. sta.ssid / sta.password 是**定长数组**（uint8_t[32] / uint8_t[64]），
     *    不是指针（见 esp_wifi_types_generic.h）。所以要先 memset 清零
     *    再 memcpy——清零保证两件事：数组尾部自动成为字符串结束符，
     *    以及其余字段（scan_method、threshold、sae_pwe_h2e 等）都取默认值 0。
     *
     *    为什么其它字段留 0 就行？
     *      threshold.authmode = 0 即 WIFI_AUTH_OPEN，含义是"不限制加密方式"，
     *      驱动会在密码长度 >= 8 时自动按 WPA2 处理（官方示例的注释也是这么
     *      说的），所以开放网络、WPA2、老式 WPA 都能连，不需要再加配置项。
     *      sae_pwe_h2e = 0 即 WPA3_SAE_PWE_UNSPECIFIED，驱动会取自己的
     *      默认值（WPA3_SAE_PWE_BOTH）。
     *
     * 2. memcpy 的长度用 strlen，绝不能用 sizeof：sizeof 会把源字符串末尾
     *    之后的垃圾字节一起拷进来，甚至读越界——这是定长数组和字符串混用时
     *    最常见的 bug。长度上限上面已经校验过（32 / 64），
     *    所以这里也不需要用 strncpy 做截断。
     */
    wifi_config_t wifi_cfg;
    memset(&wifi_cfg, 0, sizeof(wifi_cfg));
    memcpy(wifi_cfg.sta.ssid, config->ssid, ssid_len);
    memcpy(wifi_cfg.sta.password, config->password, password_len);

    /*
     * 注意 esp_wifi_set_config() 的原型是
     *     esp_err_t esp_wifi_set_config(wifi_interface_t interface,
     *                                   wifi_config_t *conf);
     * 第二个参数**不是 const**——驱动可能就地修改它，
     * 所以必须传一个可写的左值，不能传 const 指针。
     */
    ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "设置 WiFi 配置失败: %s", esp_err_to_name(ret));
        unwind_partial_init(true, true);
        return ret;
    }

    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "启动 WiFi 驱动失败: %s", esp_err_to_name(ret));
        unwind_partial_init(true, true);
        return ret;
    }

    s_connect_timeout_ms = config->connect_timeout_ms;
    s_initialized = true;

    /*
     * 走到这里**还没有连上**，这是正常的：驱动就绪后会发出
     * WIFI_EVENT_STA_START，连接在那个回调里发起。
     * 这里绝不直接调 esp_wifi_connect()，理由见 on_wifi_event()。
     */
    ESP_LOGI(TAG, "WiFi 初始化完成，等待连接");
    return ESP_OK;
}

esp_err_t wifi_mgr_wait_connected(uint32_t timeout_ms)
{
    /*
     * 未初始化时直接返回 INVALID_STATE，而不是去等一个 NULL 句柄——
     * xEventGroupWaitBits(NULL, ...) 会触发 FreeRTOS 的断言并 abort，
     * 那正是本组件要避免的"辅助功能失败拖垮整个设备"。
     */
    if (!s_initialized || s_event_group == NULL) {
        ESP_LOGE(TAG, "尚未初始化，请先调用 wifi_mgr_init()");
        return ESP_ERR_INVALID_STATE;
    }

    /* 传 0 表示"用初始化时配置的超时"（见 wifi_mgr.h 的说明） */
    const uint32_t wait_ms = (timeout_ms == 0) ? s_connect_timeout_ms : timeout_ms;

    /*
     * 两个 pdFALSE 的含义：
     *   xClearOnExit = pdFALSE —— 拿到标志位后不清零，这样之后
     *     再调 is_connected() 还能读到"已连接"。
     *   xWaitForAllBits = pdFALSE —— 只等一位，两者其实无差别，
     *     显式写出来是为了说明意图。
     *
     * 超时换算的坑：pdMS_TO_TICKS() 是向下取整的整数除法，本工程 tick
     * 频率 100Hz，1 tick = 10ms，所以 wait_ms 传 1~9 会算成 0 个 tick，
     * 等于"不等待、立刻超时"，而且不会报任何错。
     */
    const EventBits_t bits = xEventGroupWaitBits(s_event_group,
                                                WIFI_MGR_CONNECTED_BIT,
                                                pdFALSE,
                                                pdFALSE,
                                                pdMS_TO_TICKS(wait_ms));

    if ((bits & WIFI_MGR_CONNECTED_BIT) != 0) {
        return ESP_OK;
    }

    /* 走到这里可能是：路由器不可达 / 密码错 / 断线后一直在重连但还没拿到 IP */
    ESP_LOGW(TAG, "等待获取 IP 超时（%u ms），当前未联网", (unsigned)wait_ms);
    return ESP_ERR_TIMEOUT;
}

bool wifi_mgr_is_connected(void)
{
    /*
     * 未初始化（或初始化失败回滚过）时没有事件组可读，直接回答"没连上"。
     * 这里不打日志：本函数会被周期性调用（比如上报任务每轮都查一次），
     * 在日志里留下噪音反而会掩盖真正的问题。
     */
    if (s_event_group == NULL) {
        return false;
    }

    /*
     * xEventGroupGetBits() 在 FreeRTOS 里就是 xEventGroupClearBits(group, 0)
     * ——"清掉 0 个位"即纯读取，不改变任何状态。
     * 它只能在任务上下文调用（ISR 里得用 xEventGroupGetBitsFromISR），
     * 本组件的调用方都是任务，符合要求。
     */
    return (xEventGroupGetBits(s_event_group) & WIFI_MGR_CONNECTED_BIT) != 0;
}
