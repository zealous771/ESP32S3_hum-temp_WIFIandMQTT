/*
 * main.c —— 工程入口
 *
 * 这个文件的职责只有一个：把系统搭起来，然后让各个任务自己跑。
 * 具体功能不写在这里——那是组件的职责。
 *
 * 对照 HAL：HAL 里 main() 是你的主战场，while(1) 里塞满了轮询逻辑。
 * ESP-IDF 里 app_main() 只是"启动脚本"，它本身也是一个 FreeRTOS 任务
 * （叫 main 任务），跑完就该退出，把舞台交给业务任务。
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"

#include "led.h"

static const char *TAG = "main";

/*
 * LED 连接的 GPIO 编号。
 *
 * 这里用 #define 是为了让模板保持简单。更"IDF 风格"的做法是做成 Kconfig 选项，
 * 那样可以在 menuconfig 里改而不用动代码——官方 blink 示例就是这么做的：
 *   IDF_PATH/examples/get-started/blink/main/Kconfig.projbuild
 * 等你熟悉了这套流程再改造成 Kconfig 也不迟。
 *
 * 选引脚前必读：ESP32-S3 上不少引脚被固定用途占用，接了外设会出问题。
 *
 *   GPIO26 - 32    SPI0/1 总线，接板载 Flash
 *   GPIO33 - 37    八线（Octal）PSRAM 的额外数据线
 *                  —— 你的板子是 N16R8，PSRAM 是 8MB 八线模式，这 5 个脚被占用
 *   GPIO19 / 20    USB-JTAG（你调试和烧录用的就是这个口）
 *   GPIO43 / 44    UART0，也就是你的串口控制台
 *   GPIO0 / 3 / 45 / 46   strapping 引脚，上电时的电平决定启动模式，别乱接
 *
 * 安全的选择：GPIO1、2、4-18、21、38-42、47、48。
 * 具体以你板子的原理图为准。
 *
 * 关于板载 LED：很多 ESP32-S3 开发板的板载 LED 是"可寻址 RGB"（WS2812 一类），
 * 那种 LED 用普通 GPIO 高低电平驱动不了——它需要按特定时序发送数据帧，
 * 得用 RMT 或 SPI 外设。官方示例见：
 *   IDF_PATH/examples/peripherals/rmt/led_strip
 *   IDF_PATH/examples/get-started/blink   （它同时支持普通 LED 和可寻址 LED）
 * 本模板用的是普通 GPIO 驱动方式，适合外接 LED，或板载就是普通 LED 的情况。
 * 如果你板载是可寻址 RGB，代码能正常跑、串口会正常打印，但 LED 不会亮。
 */
#define LED_GPIO 2

/* 闪烁周期（毫秒） */
#define BLINK_PERIOD_MS 1000

/**
 * @brief 闪烁任务
 *
 * 单独开一个任务来做业务，而不是在 app_main 里 while(1)。
 * 这样 app_main 只负责"搭台"，业务跑在自己的任务里。
 * 将来功能变多时，每个功能一个任务，用优先级和延时让它们协作。
 */
static void blink_task(void *arg)
{
    /* 用不到的参数显式丢弃，避免编译告警 */
    (void)arg;

    ESP_LOGI(TAG, "闪烁任务启动");

    while (1) {
        ESP_ERROR_CHECK(led_toggle());
        ESP_LOGI(TAG, "LED %s", led_is_on() ? "亮" : "灭");

        /*
         * vTaskDelay() 与 HAL_Delay() 的本质区别：
         *
         * HAL_Delay() 是忙等——CPU 在这 1 秒里空转，什么也干不了。
         * vTaskDelay() 是阻塞让出——本任务进入阻塞态，调度器立刻去跑
         * 其他就绪任务，1 秒后本任务被唤醒。所以多个任务可以"同时"
         * 各等各的，互不干扰。
         *
         * 用 pdMS_TO_TICKS() 把毫秒换算成系统节拍数。当前 tick 频率是 100Hz
         * （CONFIG_FREERTOS_HZ=100），所以 1000ms = 100 个 tick。
         */
        vTaskDelay(pdMS_TO_TICKS(BLINK_PERIOD_MS));

        /*
         * 栈使用情况。返回的是"历史最低剩余栈空间"，单位是字（4 字节）。
         * 这个值越接近 0 越危险——说明栈快用满了。
         * 调任务栈大小时靠它判断，别靠猜。
         */
        ESP_LOGI(TAG, "任务栈历史最低剩余: %u 字（约 %u 字节）",
                 (unsigned)uxTaskGetStackHighWaterMark(NULL),
                 (unsigned)uxTaskGetStackHighWaterMark(NULL) * (unsigned)sizeof(StackType_t));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "app_main 开始，当前运行在 main 任务中");

    /* ESP_ERROR_CHECK：出错就打印错误码并复位，而不是带着坏状态继续跑。
     * 开发阶段建议所有可能失败的初始化都套上它，能省掉大量排查时间。 */
    ESP_ERROR_CHECK(led_init(LED_GPIO));

    /*
     * 创建闪烁任务。
     *
     * 这里有个容易踩的坑：栈深度参数的单位是【字节】，不是"字"。
     * 标准 FreeRTOS 里这个参数是字数，ESP-IDF 改成了字节
     * （头文件注释原文：Note that this differs from vanilla FreeRTOS）。
     * 所以下面 2048 表示约 2KB 栈，而不是 8KB。
     */
    BaseType_t ret = xTaskCreate(blink_task,   /* 任务函数 */
                                 "blink",      /* 任务名，调试时能在任务列表里看到 */
                                 2048,         /* 栈大小，单位字节（约 2KB） */
                                 NULL,         /* 传给任务的参数，本例不需要 */
                                 5,            /* 优先级，数字越大越高，空闲任务是 0 */
                                 NULL);        /* 任务句柄，不需要就传 NULL */

    /* xTaskCreate 返回 pdPASS 表示创建成功。创建失败通常是内存不够。 */
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "创建闪烁任务失败");
        return;
    }

    /*
     * app_main 在这里返回。
     *
     * 返回不等于程序结束。main 任务会自行删除，
     * 而 blink_task 会继续跑下去。
     * 这和 HAL 里 main() 返回即"程序结束"完全不同——
     * 在 ESP-IDF 里，只要还有任务在跑，系统就活着。
     */
    ESP_LOGI(TAG, "app_main 返回，main 任务即将结束，blink_task 继续运行");
}
