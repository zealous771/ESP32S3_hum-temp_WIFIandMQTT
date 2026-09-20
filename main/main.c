/*
 * main.c —— 工程入口
 *
 * 这个文件只做两件事：
 *   1. 声明板级配置（引脚、周期表）—— 换板子只改这里
 *   2. 调用 app_start() 把控制权交给应用层
 *
 * 业务逻辑不在这里，在 components/app/ 下：
 *   app.c           编排：初始化外设、建队列、创建任务
 *   blink_task.c    闪烁任务
 *   button_task.c   按键任务
 *
 * 为什么这样分：main 在 ESP-IDF 里是个特殊组件（总是被编译、总是第一个运行）。
 * 如果把任务都堆在这里，每加一个功能入口文件就长一截，很快读不动了。
 * 拆开之后，加新任务只改 app 组件，这个文件保持不变。
 *
 * 数据流（跨四个组件）：
 *
 *   按键 GPIO ──中断──▶ button 组件 ISR ──队列──▶ app/button_task
 *                                                      │
 *                                                  队列（新周期）
 *                                                      ▼
 *                        led 组件 ◀── app/blink_task ◀──┘
 *
 *   RGB GPIO ◀── RMT 外设 ◀── rgb_led 组件 ◀── app/rgb_task（独立循环色相）
 */
#include <stdint.h>

#include "esp_err.h"

#include "app.h"

/* ---------- 板级配置 ---------- */

/*
 * 外接 LED 的引脚。板载 LED1/LED2 接在 UART 线上（U0TXD/U0RXD），
 * 那是串口收发指示灯，不适合当用户 LED 用，所以这里用外接 LED。
 * 安全可用的 GPIO：1、2、4-18、21、38-42、47、48。
 * 注意 GPIO33-37 被八线 PSRAM 占用，GPIO26-32 是 Flash 总线。
 */
#define LED_GPIO 2

/*
 * 按键引脚。KEY2 就是 BOOT 按键，接 GPIO0，低电平有效。
 *
 * 注意 GPIO0 是 strapping 引脚：上电瞬间它的电平决定芯片是否进入下载模式。
 * 作为运行期输入完全没问题，但不要在复位时按住它，
 * 否则芯片会停在 ROM 下载模式，程序不启动。
 */
#define BUTTON_GPIO         0
#define BUTTON_ACTIVE_LEVEL 0     /* 0 = 低电平有效（按键接地） */
#define BUTTON_DEBOUNCE_MS  30

/*
 * 板载可寻址 RGB（WS2812B）的数据线，接在 GPIO48。
 *
 * 注意这颗灯**不能**用普通 GPIO 的高低电平驱动——它是一根单线串行总线，
 * 靠高电平持续的长短来编码 0 和 1，时序要求到百纳秒量级。
 * 所以它走的是 RMT 外设，由 rgb_led 组件负责，
 * 详见 components/rgb_led/include/rgb_led.h 里的说明。
 */
#define RGB_GPIO       48
#define RGB_LED_COUNT  1

/* 闪烁周期可选项，按键按下时循环切换 */
static const uint32_t k_periods_ms[] = {1000, 500, 200, 100};

void app_main(void)
{
    const app_config_t cfg = {
        .led_gpio            = LED_GPIO,
        .button_gpio         = BUTTON_GPIO,
        .button_active_level = BUTTON_ACTIVE_LEVEL,
        .button_debounce_ms  = BUTTON_DEBOUNCE_MS,
        .rgb_gpio            = RGB_GPIO,
        .rgb_led_count       = RGB_LED_COUNT,
        .periods_ms          = k_periods_ms,
        .period_count        = sizeof(k_periods_ms) / sizeof(k_periods_ms[0]),
    };

    /*
     * app_start() 返回时各任务已在运行，app_main 可以直接返回——
     * app_main 返回不等于程序结束，main 任务会被 FreeRTOS 自行删除。
     */
    ESP_ERROR_CHECK(app_start(&cfg));
}
