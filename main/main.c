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

#include "sdkconfig.h"

#include "esp_err.h"

#include "app.h"

/*
 * 闪烁周期可选项，按键按下时循环切换。
 *
 * 这个没有做成 Kconfig 选项——Kconfig 擅长标量和布尔值，
 * 表达"一个变长列表"很别扭。硬编码在代码里反而更清楚。
 */
static const uint32_t k_periods_ms[] = {1000, 500, 200, 100};

void app_main(void)
{
    /*
     * 把配置装起来交给应用层。
     *
     * 注意这里 app 组件**完全不知道**这些值是什么——引脚接在哪、
     * 去抖多久，都是"这台机器特有的信息"，由调用方注入。
     * 所以 components/app/ 整个目录可以原样复制到别的工程。
     */
    const app_config_t cfg = {
        .led_gpio            = CONFIG_BOARD_LED_GPIO,
        .button_gpio         = CONFIG_BOARD_BUTTON_GPIO,
        .button_active_level = CONFIG_BOARD_BUTTON_ACTIVE_LEVEL,
        .button_debounce_ms  = CONFIG_BOARD_BUTTON_DEBOUNCE_MS,
        .rgb_gpio            = CONFIG_BOARD_RGB_GPIO,
        .rgb_led_count       = CONFIG_BOARD_RGB_LED_COUNT,
        .periods_ms          = k_periods_ms,
        .period_count        = sizeof(k_periods_ms) / sizeof(k_periods_ms[0]),
    };

    /*
     * app_start() 返回时各任务已在运行，app_main 可以直接返回——
     * app_main 返回不等于程序结束，main 任务会被 FreeRTOS 自行删除。
     */
    ESP_ERROR_CHECK(app_start(&cfg));
}
