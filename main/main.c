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
 * 数据流（跨五个组件）：
 *
 *   按键 GPIO ──中断──▶ button 组件 ISR ──队列──▶ app/button_task
 *                                                      │
 *                                                  队列（新周期）
 *                                                      ▼
 *                        led 组件 ◀── app/blink_task ◀──┘
 *
 *   RGB GPIO ◀── RMT 外设 ◀── rgb_led 组件 ◀── app/rgb_task（独立循环色相）
 *
 *   DHT11 数据线 ◀── 单总线时序 ◀── dht11 组件 ◀── app/telemetry_task
 *                                                      │
 *                                              JSON 报文（串口打印）
 *                                                      ▼
 *                        下一轮接 MQTT 时，这份报文直接 publish 出去
 *
 *   WiFi ◀── wifi_mgr 组件（Station 模式）──▶ telemetry_task 查连接状态
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

        .wifi_ssid               = CONFIG_WIFI_SSID,
        .wifi_password           = CONFIG_WIFI_PASSWORD,
        .wifi_connect_timeout_ms = WIFI_CONNECT_TIMEOUT_MS,

        .periods_ms          = k_periods_ms,
        .period_count        = sizeof(k_periods_ms) / sizeof(k_periods_ms[0]),
    };

    /*
     * app_start() 返回时各任务已在运行，app_main 可以直接返回——
     * app_main 返回不等于程序结束，main 任务会被 FreeRTOS 自行删除。
     */
    ESP_ERROR_CHECK(app_start(&cfg));
}
