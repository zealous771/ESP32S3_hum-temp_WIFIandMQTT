/*
 * main.c —— 工程入口
 *
 * 这个文件只做两件事：
 *   1. 把板级配置（引脚、周期表）装进 app_config_t
 *   2. 调用 app_start() 把控制权交给应用层
 *
 * 业务逻辑不在这里，在 components/app/ 下：
 *   app.c           编排：初始化外设、建队列、创建任务
 *   blink_task.c    闪烁任务
 *   button_task.c   按键任务
 *   rgb_task.c      可寻址 RGB 任务
 *   sensor_task.c   温湿度采样任务（生产者：只采集，不输出）
 *   console_task.c  串口消费者（把记录打成 JSON，不依赖网络）
 *   mqtt_task.c     MQTT 消费者（把记录 publish 到 broker）
 *
 * 为什么这样分：main 在 ESP-IDF 里是个特殊组件（总是被编译、总是第一个运行）。
 * 如果把任务都堆在这里，每加一个功能入口文件就长一截，很快读不动了。
 * 拆开之后，加新任务只改 app 组件，这个文件保持不变。
 *
 * 板级配置从哪来？
 *
 *   引脚号、去抖时间这些**不再写死在代码里**，而是定义在同目录的
 *   main/Kconfig.projbuild 里。menuconfig 里改一下就行，不用动源码。
 *
 *   读法是直接用 CONFIG_ 开头的宏——它们来自构建系统生成的 sdkconfig.h。
 *   注意 IDF **不会**自动包含那个头文件：编译命令里既没有 -include
 *   sdkconfig.h，也没有 -DCONFIG_xxx。所以下面必须显式 include。
 *   （生成位置是 build/config/sdkconfig.h，该目录已在头文件搜索路径里。）
 *
 * 数据流（跨六个组件）：
 *
 *   按键 GPIO ──中断──▶ button 组件 ISR ──队列──▶ app/button_task
 *                                                      │
 *                                                  队列（新周期）
 *                                                      ▼
 *                        led 组件 ◀── app/blink_task ◀──┘
 *
 *   RGB GPIO ◀── RMT 外设 ◀── rgb_led 组件 ◀── app/rgb_task（独立循环色相）
 *
 *   ┌────────── 采样管道（一个生产者 / 两个消费者）────────────┐
 *   │                                                          │
 *   │  DHT11 数据线 ◀── 单总线时序 ◀── dht11 组件               │
 *   │                                    │                     │
 *   │                              app/sensor_task             │
 *   │                              （只采样，绝不阻塞）          │
 *   │                                    │                     │
 *   │                     ┌──────────────┴──────────────┐      │
 *   │                     ▼                             ▼      │
 *   │            Queue console_queue          Queue mqtt_queue │
 *   │                     │                             │      │
 *   │             app/console_task                app/mqtt_task│
 *   │             （格式化成 JSON 打串口）      （publish 到    │
 *   │              不依赖网络，断网时照常打印      broker）     │
 *   └──────────────────────────────────────────────────────────┘
 *
 *   为什么是两条队列而不是一条加两个消费者：xQueueReceive 是**出队**
 *   语义，两个任务读同一条队列会变成瓜分数据而不是广播。
 *   详见 components/app/src/app_internal.h。
 *
 *   两个消费者还有一个刻意的行为差异：mqtt_task 在未连接时**不消费**
 *   队列（让它积压，重连后补发），而 console_task 永远在消费。
 *   于是"网络断了导致丢数据"只影响上报那一路，串口照常一行一行地打——
 *   两种故障因此可以被区分开。
 *
 *   WiFi ◀── wifi_mgr 组件（Station 模式）
 *   NTP  ◀── time_mgr 组件（SNTP，把开机毫秒锚定成 Unix 毫秒）
 *   broker ◀── mqtt_mgr 组件（连接管理 + publish，带 LWT 在线状态）
 */
#include <stdint.h>

#include "sdkconfig.h"

#include "esp_err.h"
#include "esp_log.h"

#include "app.h"

/* 日志标签。约定用文件名，方便在串口输出里过滤。 */
static const char *TAG = "main";

/*
 * 闪烁周期可选项，按键按下时循环切换。
 *
 * 这个没有做成 Kconfig 选项——Kconfig 擅长标量和布尔值，
 * 表达"一个变长列表"很别扭。硬编码在代码里反而更清楚。
 */
static const uint32_t k_periods_ms[] = {1000, 500, 200, 100};

/*
 * 等待拿到 IP 的超时。
 *
 * 这个不是"板级信息"（跟引脚无关），所以留在代码里而不是 Kconfig。
 * 15 秒足够覆盖路由器 DHCP 较慢的情况；超时也不影响设备其它功能，
 * 只是串口里不会出现 IP 那一行。
 */
#define WIFI_CONNECT_TIMEOUT_MS 15000

/*
 * SSID 没填时提醒一句。
 *
 * 为什么是**运行期**判断而不是 #if 编译期判断？
 *   CONFIG_WIFI_SSID 是 Kconfig 的 string 类型，展开后是字符串字面量
 *   （"" 或 "MyAP" 这样）。而 C 的预处理器**不认识字符串字面量**：
 *   写 `#if CONFIG_WIFI_SSID[0] == '\0'` 会直接报
 *       error: token '""' is not valid in preprocessor expressions
 *   整个编译都过不去（本工程实测撞过这个坑）。
 *   sizeof() 虽然能进 #if，但它不能作用于 #if 里的字符串比较；
 *   总之"字符串内容"这类判断天生属于运行期。
 *
 * 为什么值得提醒？因为"忘了填 SSID"的表现是——编译、烧录、运行全部正常，
 * 只是永远连不上，串口里一个错都不报。这种静默失败最费排查时间。
 */
static void warn_if_ssid_empty(void)
{
    if (CONFIG_WIFI_SSID[0] == '\0') {
        ESP_LOGW(TAG, "未配置 WiFi 名称（CONFIG_WIFI_SSID 为空），本次不联网；"
                      "填法：idf.py menuconfig → WiFi 连接");
    }
}

void app_main(void)
{
    warn_if_ssid_empty();

    /*
     * 把配置装起来交给应用层。
     *
     * 注意这里 app 组件**完全不知道**这些值是什么——引脚接在哪、
     * 去抖多久、连哪个 AP，都是"这台机器特有的信息"，由调用方注入。
     * 所以 components/app/ 整个目录可以原样复制到别的工程。
     */
    const app_config_t cfg = {
        .led_gpio            = CONFIG_BOARD_LED_GPIO,
        .led_active_level    = CONFIG_BOARD_LED_ACTIVE_LEVEL,
        .button_gpio         = CONFIG_BOARD_BUTTON_GPIO,
        .button_active_level = CONFIG_BOARD_BUTTON_ACTIVE_LEVEL,
        .button_debounce_ms  = CONFIG_BOARD_BUTTON_DEBOUNCE_MS,
        .rgb_gpio            = CONFIG_BOARD_RGB_GPIO,
        .rgb_led_count       = CONFIG_BOARD_RGB_LED_COUNT,

        .dht11_gpio          = CONFIG_BOARD_DHT11_GPIO,
        .sample_interval_ms  = CONFIG_BOARD_SAMPLE_INTERVAL_MS,

        /*
         * 设备标识。和引脚一样属于"这台机器是谁"，所以也走 Kconfig，
         * 而不是在组件里写死或用 MAC 拼一个——那样等于替调用方做了
         * 一个它不该做的命名决定。
         */
        .device_id           = CONFIG_DEVICE_ID,

        .wifi_ssid               = CONFIG_WIFI_SSID,
        .wifi_password           = CONFIG_WIFI_PASSWORD,
        .wifi_connect_timeout_ms = WIFI_CONNECT_TIMEOUT_MS,

        /*
         * NTP 服务器。留空表示不做时间同步，报文时间戳为开机毫秒
         * （clock 字段标成 "boot"）——这是受支持的用法，不是错误。
         */
        .ntp_server              = CONFIG_NTP_SERVER,

        /*
         * MQTT 上报。uri 留空表示不启用上报（受支持的用法）——
         * 设备照常采集并往串口打报文，只是不发给 broker。
         *
         * 和 WiFi 凭据一样，真实地址/密码只写进本机 sdkconfig；
         * sdkconfig.defaults 里这几项**故意留空**。
         */
        .mqtt_uri                 = CONFIG_MQTT_BROKER_URI,
        .mqtt_username            = CONFIG_MQTT_USERNAME,
        .mqtt_password            = CONFIG_MQTT_PASSWORD,
        .mqtt_client_id_prefix    = CONFIG_MQTT_CLIENT_ID_PREFIX,
        .mqtt_keepalive_s         = CONFIG_MQTT_KEEPALIVE_S,
        .mqtt_reconnect_timeout_ms = CONFIG_MQTT_RECONNECT_TIMEOUT_MS,
        .mqtt_net_timeout_ms      = CONFIG_MQTT_NET_TIMEOUT_MS,

        .periods_ms          = k_periods_ms,
        .period_count        = sizeof(k_periods_ms) / sizeof(k_periods_ms[0]),
    };

    /*
     * app_start() 返回时各任务已在运行，app_main 可以直接返回——
     * app_main 返回不等于程序结束，main 任务会被 FreeRTOS 自行删除。
     */
    ESP_ERROR_CHECK(app_start(&cfg));
}
