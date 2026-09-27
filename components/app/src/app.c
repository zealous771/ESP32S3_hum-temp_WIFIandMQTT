/*
 * app.c —— 应用编排层
 *
 * 这个文件只做"编排"，不含业务逻辑：
 *   1. 初始化外设
 *   2. 建立任务间通信的通道（队列）
 *   3. 创建各任务
 *
 * 各任务的具体实现在同目录的 *_task.c 里。这样加新任务时，
 * 本文件只需要增加一次 xTaskCreate()，不会随功能增长而无限变长——
 * 这正是把任务从 main.c 拆出来的目的。
 */
#include "app.h"
#include "app_internal.h"

#include <stdio.h>

#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"

#include "button.h"
#include "dht11.h"
#include "led.h"
#include "mqtt_mgr.h"
#include "rgb_led.h"
#include "settings.h"
#include "time_mgr.h"
#include "wifi_mgr.h"

static const char *TAG = "app";

/*
 * 上下文必须比所有任务活得久，所以放静态存储区。
 * 不能用 app_start() 的局部变量——函数返回后栈就失效了，而任务还在跑。
 */
static app_ctx_t s_ctx;

/*
 * 任务参数集中在这里。调整栈大小或优先级不用翻遍各个任务文件。
 *
 * 栈的单位是**字节**，不是字。这是 ESP-IDF 与标准 FreeRTOS 的差异，
 * 头文件里的原文是 "NUMBER OF BYTES. Note that this differs from vanilla FreeRTOS."
 */
#define BLINK_TASK_NAME    "blink"
#define BLINK_TASK_STACK   2048
#define BLINK_TASK_PRIO    5

#define BUTTON_TASK_NAME   "button"
#define BUTTON_TASK_STACK  3072
#define BUTTON_TASK_PRIO   5

#define RGB_TASK_NAME      "rgb"
#define RGB_TASK_STACK     2560
#define RGB_TASK_PRIO      5

/*
 * 遥测管道拆成两个任务：生产者（采样）和消费者（输出）。
 *
 * 栈大小不再对称，这是拆分带来的直接收益——
 * 拆分前一个任务要同时承担"读传感器"和"格式化 JSON + 打日志"，
 * 只能按最费栈的那一半来给。现在两边各按自己的实际需要给：
 *
 *   sensor  读单总线 + 填结构体 + 拷进队列，不碰格式化，可以给得小
 *   console 每轮走一遍 snprintf + ESP_LOGI，格式化是**整个 app 里
 *           最吃栈的操作**（snprintf 内部上百字节转换缓冲，
 *           日志后端还会再格式化一次，中断也可能在此期间嵌套），
 *           所以它要得最多
 *
 * 具体数值不靠猜——两个任务都周期性调用 app_report_stack() 上报水位，
 * 按实测值再调。经验判据是余量留 30% 以上。
 */
#define SENSOR_TASK_NAME   "sensor"
#define SENSOR_TASK_STACK  2560
#define SENSOR_TASK_PRIO   5

#define CONSOLE_TASK_NAME  "console"
#define CONSOLE_TASK_STACK 3584
#define CONSOLE_TASK_PRIO  5

/*
 * 采样队列深度。
 *
 * 深度决定"消费者能落后多久"。消费者按 2 秒一条的速度消费，
 * 深度 8 意味着消费者可以卡住约 16 秒而不丢数据——足够覆盖
 * 一次短暂的网络抖动或日志阻塞。
 *
 * 为什么不做得很深（比如 100）：队列元素是**按值拷贝**的，
 * 深度直接乘上 sizeof(sensor_data_t)（约 56 字节）就是常驻内存。
 * 而队列真正的职责是"吸收抖动"，不是"长期囤数据"——
 * 消费者要是持续跟不上，囤多少都不够，这时正确的做法是
 * 落盘（NVS/Flash，见文档 §37）或者干脆丢弃，而不是把 RAM 吃光。
 * 在只有 512KB 内部 SRAM 的 S3 上，"加深队列"是个会反噬的选项。
 */
#define SENSOR_QUEUE_DEPTH 8

/*
 * 采样周期的兜底下限。
 *
 * 为什么要防这一手：如果这里传进来 0，vTaskDelay(0) 不阻塞，
 * 任务会变成紧循环——一边狂刷 DHT11 总线，一边以最高速度打日志，
 * 既会把串口冲垮，也会饿死同优先级的其它任务。
 *
 * 2000 不是随手取的数：DHT11 手册要求两次读取间隔大于 2 秒。
 */
#define SAMPLE_INTERVAL_MIN_MS 2000

/*
 * client_id 的拼装缓冲。
 *
 * MQTT 规范要求 client_id 在同一个 broker 上唯一——两个设备用同一个
 * client_id 连接，后连上的会把先连上的**踢下线**，表现为两台设备轮流掉线，
 * 而且日志里只有一条语焉不详的 DISCONNECTED。
 *
 * 所以默认用 device_id 当 client_id（它本来就该是唯一的），
 * 另外给一个前缀开关：前缀为空时直接用 device_id（默认，最省事），
 * 非空时拼成 "<前缀>_<device_id>"——用于同一台设备跑两份固件做对比测试，
 * 数据要归到同一台设备名下但连接标识不能撞的场景。
 *
 * 必须是 static：mqtt_mgr 会把指针交给 esp-mqtt，esp-mqtt 长期持有它
 * （详见 mqtt_mgr.h 的 @note）。放栈上会留下一根悬空指针。
 */
#define CLIENT_ID_MAX 80
static char s_client_id[CLIENT_ID_MAX];

/**
 * @brief 按前缀规则拼出 client_id
 *
 * @return 始终返回一个全程存活的字符串：前缀为空时就是 device_id 本身
 *         （它来自 Kconfig，生命周期是全程的）
 */
static const char *compose_client_id(const char *prefix, const char *device_id)
{
    if (prefix == NULL || prefix[0] == '\0') {
        return device_id;
    }

    const int n = snprintf(s_client_id, sizeof(s_client_id), "%s_%s",
                           prefix, device_id);
    if (n < 0 || n >= (int)sizeof(s_client_id)) {
        /*
         * 截断的 client_id 比不拼更危险：两台设备的前缀部分相同、
         * device_id 又被截掉，就撞成一个 id 了。所以退回 device_id。
         */
        ESP_LOGW(TAG, "client_id 前缀过长，改用 device_id 作为 client_id");
        return device_id;
    }
    return s_client_id;
}

static esp_err_t init_peripherals(const app_config_t *config)
{
    const led_config_t led_cfg = {
        .gpio_num     = config->led_gpio,
        .active_level = config->led_active_level,
    };
    esp_err_t ret = led_init(&led_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LED 初始化失败: %s", esp_err_to_name(ret));
        return ret;
    }

    const button_config_t btn_cfg = {
        .gpio_num     = config->button_gpio,
        .active_level = config->button_active_level,
        .debounce_ms  = config->button_debounce_ms,
    };
    ret = button_init(&btn_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = rgb_led_init(config->rgb_gpio, config->rgb_led_count);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "RGB 初始化失败: %s", esp_err_to_name(ret));
        return ret;
    }

    const dht11_config_t dht_cfg = {
        .gpio_num        = config->dht11_gpio,
        .min_interval_ms = config->sample_interval_ms,
    };
    ret = dht11_init(&dht_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "DHT11 初始化失败: %s", esp_err_to_name(ret));
        return ret;
    }

    /*
     * WiFi 放最后初始化：前面几步是"本机必须能用的功能"，
     * WiFi 是"锦上添花"。真出问题时先保证前面的已经就绪。
     *
     * 两个特殊处理：
     *
     *   ESP_ERR_INVALID_ARG —— 表示 SSID 没填。这是受支持的"不联网"用法，
     *     不是故障，所以降级继续跑，不往上返回错误。判断依据是空 SSID
     *     在 wifi_mgr 里被明确归到 INVALID_ARG；真正的初始化失败
     *     （内存不足、底层驱动报错）会走到下面的错误分支。
     *
     *   wifi_mgr_init() 不阻塞：它只注册事件、启动驱动就返回。
     *     要不要等 IP 由调用方决定。这里选择**等一下**，理由见下面
     *     调用处的注释。
     */
    const wifi_mgr_config_t wifi_cfg = {
        .ssid               = config->wifi_ssid,
        .password           = config->wifi_password,
        .connect_timeout_ms = config->wifi_connect_timeout_ms,
    };

    /*
     * 记录"网络接口是否已就绪"。
     *
     * 为什么需要这个标志：SNTP 依赖**默认事件循环**和**默认网络接口**，
     * 而那两个是 wifi_mgr_init() 建的。空 SSID 时它们根本不存在，
     * 这时若照调 esp_netif_sntp_init()，它会在初始化网络层时失败，
     * 打出一串与真正原因无关的错误。与其事后猜，不如在这里显式分流。
     */
    bool netif_ready = false;

    ret = wifi_mgr_init(&wifi_cfg);
    if (ret == ESP_ERR_INVALID_ARG) {
        ESP_LOGW(TAG, "未配置 WiFi 名称，本次不联网（其它功能照常）");
    } else if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi 初始化失败: %s", esp_err_to_name(ret));
        return ret;
    } else {
        /*
         * 等 IP，但**超时不当失败**。
         *
         * 为什么在这里等而不是让 telemetry_task 自己管？
         *   等完再建任务，遥测的第一行报文就能带上正确的 wifi 标志；
         *   否则前几秒会打出 "wifi":false，容易被误读成"连不上"。
         *
         * 为什么超时不返回错误？
         *   路由器不在、密码还没填对，这些都是**可能随时恢复**的情况：
         *   wifi_mgr 内部有重连逻辑，连上后标志位会自己变成 true。
         *   为此让 app_start() 失败（进而 ESP_ERROR_CHECK 复位）就太粗暴了——
         *   设备明明还能做本地采集。所以只记一行告警。
         *
         * 最坏代价：路由器不可达时启动多花 wifi_connect_timeout_ms。
         * 这是有界的，且换来了确定性的启动日志顺序。
         */
        ret = wifi_mgr_wait_connected(config->wifi_connect_timeout_ms);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "等待 WiFi 连接超时（%s），继续启动；连上后会自动恢复",
                     esp_err_to_name(ret));
        }

        /* 走到这个 else 分支就说明 wifi_mgr_init() 成功了，网络接口已存在 */
        netif_ready = true;
    }

    /*
     * ── 时间同步 ──────────────────────────────────────────────
     *
     * 放在 WiFi 之后，因为 SNTP 要借 WiFi 建好的网络接口和事件循环。
     *
     * **不等它同步完成**。SNTP 是异步的：要等 DNS 解析、等 UDP 往返，
     * 服务器不可达时还要退避重试，整个过程可能几秒到几十秒。
     * 在启动路径上等它，等于把"启动耗时"交给一个外部服务器的响应速度
     * 决定——这是不可接受的。同步完成时会由 time_mgr 的回调打一条日志，
     * 那时候时间戳自然就变成 Unix 毫秒了。
     *
     * 三种结果的处理都不同，注意区分：
     *
     *   ESP_ERR_INVALID_ARG —— NTP 服务器没配。这是**受支持的用法**
     *     （与空 SSID 同一模式），不是故障，所以只是提示一句。
     *     报文时间戳恒为开机毫秒，clock 字段会标成 "boot"。
     *
     *   其他错误 —— 时间同步是**辅助功能**：它失败只影响时间戳的基准，
     *     采集、上报、本地功能一个都不受影响。所以记 LOGW 不记 LOGE，
     *     更不能让它阻断启动（这里不 return）。
     *
     *   未联网 —— 没有网络接口，SNTP 无从谈起，直接跳过。
     *     这里不调 time_mgr_init()，避免它在缺 netif 时报出无关的错误。
     */
    if (netif_ready) {
        const time_mgr_config_t time_cfg = { .server = config->ntp_server };
        ret = time_mgr_init(&time_cfg);
        if (ret == ESP_ERR_INVALID_ARG) {
            ESP_LOGW(TAG, "未配置 NTP 服务器，时间戳用开机毫秒（clock=boot）");
        } else if (ret != ESP_OK) {
            ESP_LOGW(TAG, "时间同步初始化失败（%s），时间戳退回开机毫秒",
                     esp_err_to_name(ret));
        }
    } else {
        ESP_LOGW(TAG, "未联网，跳过时间同步；时间戳用开机毫秒（clock=boot）");
    }

    /*
     * ── MQTT ──────────────────────────────────────────────────
     *
     * 同样放在 WiFi 之后：esp-mqtt 要用到网络栈。
     * **不要求已经拿到 IP**——它自己会重试，网络通了自然连上。
     * 这里也**不等连接完成**：等待是 mqtt_task 的事（它反正要阻塞在
     * 队列上），启动路径不该被外部服务的响应速度拖住。
     *
     * 两种"不启用"的路径要分清：
     *
     *   ESP_ERR_INVALID_ARG —— uri 没配。受支持的用法，提示一句。
     *     此时 mqtt_mgr 的所有查询返回 "disabled"、publish 直接失败，
     *     mqtt_task 会看到状态是 disabled 并安静地什么都不做。
     *
     *   未联网 —— 没有网络接口，连不上任何 broker。跳过初始化，
     *     免得 esp-mqtt 反复报一些与真正原因无关的错误。
     *
     * 其他错误（内存不足之类）记 LOGW 不记 LOGE，也不 return：
     * 上报是辅助功能，它挂了不影响采集和本地功能。
     */
    if (netif_ready) {
        const mqtt_mgr_config_t mqtt_cfg = {
            .uri                  = config->mqtt_uri,
            .device_id            = config->device_id,
            .username             = config->mqtt_username,
            .password             = config->mqtt_password,
            .client_id            = compose_client_id(config->mqtt_client_id_prefix,
                                                      config->device_id),
            .keepalive_s          = config->mqtt_keepalive_s,
            .reconnect_timeout_ms = config->mqtt_reconnect_timeout_ms,
            .net_timeout_ms       = config->mqtt_net_timeout_ms,
        };
        ret = mqtt_mgr_init(&mqtt_cfg);
        if (ret == ESP_ERR_INVALID_ARG) {
            ESP_LOGW(TAG, "未配置 MQTT broker，本次不上报（其它功能照常）");
        } else if (ret != ESP_OK) {
            ESP_LOGW(TAG, "MQTT 初始化失败（%s），本次不上报",
                     esp_err_to_name(ret));
        }
    } else {
        ESP_LOGW(TAG, "未联网，跳过 MQTT 初始化；本次不上报");
    }

    return ESP_OK;
}

static esp_err_t create_tasks(void)
{
    /*
     * 五个任务的优先级都给 5——它们互不抢占，靠各自的阻塞调用自然协作：
     * blink 和 button 阻塞在队列上，rgb 阻塞在延时上，
     * sensor 阻塞在采样周期的延时上，console 阻塞在采样队列上。
     *
     * 如果按键要求响应更快，可以把 button_task 提到 6。
     *
     * 特别说一下 sensor 和 console 这对生产/消费者为什么同为 5 也不会出问题：
     *   两者都在阻塞调用上等待（vTaskDelay / xQueueReceive），
     *   不会出现"高优先级忙等把低优先级饿死"的经典问题。
     *   就算 sensor 被提到 6，也只是让"读传感器"更及时，
     *   因为 console 拿不到 CPU 而丢数据的情况不会发生——
     *   队列深度 8 已经吸收了这种抖动。
     *   真正要警惕的是"给生产者更高优先级 + 队列满时阻塞发送"这个组合，
     *   那会让采样任务在高优先级上把消费者彻底饿死。本工程用的是
     *   非阻塞发送（超时 0），从设计上就绕开了这个坑。
     *
     * pvParameters 传 &s_ctx：任务通过它拿到周期表、队列和设备标识，
     * 不需要访问任何全局变量。rgb_task 用不到上下文，
     * 但保持一致的传参方式，以后它要加参数时不用改调用处。
     */
    if (xTaskCreate(app_blink_task, BLINK_TASK_NAME, BLINK_TASK_STACK,
                    &s_ctx, BLINK_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "创建闪烁任务失败");
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(app_button_task, BUTTON_TASK_NAME, BUTTON_TASK_STACK,
                    &s_ctx, BUTTON_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "创建按键任务失败");
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(app_rgb_task, RGB_TASK_NAME, RGB_TASK_STACK,
                    &s_ctx, RGB_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "创建 RGB 任务失败");
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(app_sensor_task, SENSOR_TASK_NAME, SENSOR_TASK_STACK,
                    &s_ctx, SENSOR_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "创建采样任务失败");
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(app_console_task, CONSOLE_TASK_NAME, CONSOLE_TASK_STACK,
                    &s_ctx, CONSOLE_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "创建串口消费者任务失败");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void app_report_stack(const char *tag)
{
    /*
     * uxTaskGetStackHighWaterMark(NULL) 返回**当前任务**历史最低的剩余栈空间。
     * 传 NULL 就是"我自己"。
     *
     * 单位是字节——ESP-IDF 把 portSTACK_TYPE 定义成了 uint8_t，
     * 所以这里不需要再换算。详见 app_internal.h 的说明。
     *
     * 怎么用这个值：
     *   它越接近 0 越危险。留个 30% 以上的余量比较稳妥，
     *   因为中断嵌套、日志格式化、浮点运算都可能临时吃掉更多栈。
     *   调 xTaskCreate 的栈参数时靠它判断，别靠猜。
     */
    const uint32_t free_bytes = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI(tag, "栈水位：历史最低剩余 %u 字节", (unsigned)free_bytes);
}

esp_err_t app_start(const app_config_t *config)
{
    if (config == NULL || config->periods_ms == NULL || config->period_count == 0 ||
        config->device_id == NULL) {
        ESP_LOGE(TAG, "配置非法");
        return ESP_ERR_INVALID_ARG;
    }

    s_ctx.periods_ms   = config->periods_ms;
    s_ctx.period_count = config->period_count;
    s_ctx.device_id    = config->device_id;

    /*
     * 采样周期兜底。
     *
     * Kconfig 里已经用 range 2000 60000 拦住了非法值，但 app 组件是
     * "可以被复制到别的工程"的，不能假设调用方一定走了那份 Kconfig。
     * 传 0 的后果不是"读得快一点"，而是任务变成紧循环把总线刷爆，
     * 所以在组件边界上再挡一道——这类校验放在组件里比放在入口里可靠。
     */
    s_ctx.sample_interval_ms = config->sample_interval_ms;
    if (s_ctx.sample_interval_ms < SAMPLE_INTERVAL_MIN_MS) {
        ESP_LOGW(TAG, "采样周期 %u ms 过短，改用 %u ms（DHT11 要求 > 2 秒）",
                 (unsigned)s_ctx.sample_interval_ms,
                 (unsigned)SAMPLE_INTERVAL_MIN_MS);
        s_ctx.sample_interval_ms = SAMPLE_INTERVAL_MIN_MS;
    }

    /*
     * 先从 NVS 读回上次保存的周期，在周期表里反查出下标。
     *
     * 为什么存的是周期值（ms）而不是下标？
     *   周期表是代码里的常量，将来可能改动。如果存下标，改了表之后
     *   旧的下标会指向错误的档位（甚至越界）；存周期值的话，
     *   查不到就退回第 0 档，行为始终可预期。
     *   这类"持久化数据与代码里的表耦合"的问题很常见，
     *   原则是：**存语义稳定的值，不存位置**。
     */
    esp_err_t ret = settings_init();
    if (ret != ESP_OK) {
        return ret;
    }

    const uint32_t saved_ms = settings_get_period_ms(config->periods_ms[0]);
    uint32_t initial_index = 0;
    for (size_t i = 0; i < config->period_count; i++) {
        if (config->periods_ms[i] == saved_ms) {
            initial_index = i;
            break;
        }
    }
    if (config->periods_ms[initial_index] != saved_ms) {
        ESP_LOGW(TAG, "保存的周期 %u ms 不在当前周期表里，改用 %u ms",
                 (unsigned)saved_ms, (unsigned)config->periods_ms[0]);
    }
    s_ctx.initial_period_index = initial_index;

    ret = init_peripherals(config);
    if (ret != ESP_OK) {
        return ret;
    }

    /* 深度 4 够用：只有按键按下时写一次，而 blink_task 每次都读走 */
    s_ctx.period_queue = xQueueCreate(4, sizeof(uint32_t));
    if (s_ctx.period_queue == NULL) {
        ESP_LOGE(TAG, "创建周期队列失败");
        return ESP_ERR_NO_MEM;
    }

    /*
     * 采样队列。这是"采样与输出解耦"的物理载体——
     * 生产者（sensor_task）和消费者（console_task / 未来的 mqtt_task）
     * 之间唯一的连接点，两侧互相不知道对方的存在。
     *
     * 元素大小是 sizeof(sensor_data_t)，按值拷贝。队列一旦创建，
     * 元素大小就固定了，所以 sensor_data_t 以后加字段（比如 rssi）
     * 不需要改这里——sizeof 会自动跟着变。
     */
    s_ctx.sensor_queue = xQueueCreate(SENSOR_QUEUE_DEPTH, sizeof(sensor_data_t));
    if (s_ctx.sensor_queue == NULL) {
        ESP_LOGE(TAG, "创建采样队列失败");
        return ESP_ERR_NO_MEM;
    }

    ret = create_tasks();
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_LOGI(TAG, "应用启动完成");
    return ESP_OK;
}
