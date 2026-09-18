/*
 * main.c —— 工程入口
 *
 * 本示例演示 ESP-IDF 的"中断 + 任务间通信"标准模式：
 *
 *     按键 GPIO ──中断──▶ ISR ──队列──▶ button_task ──队列──▶ blink_task ──▶ LED
 *
 * 两个队列各自承担不同职责：
 *   button 组件内部的队列：传递"按下/松开"事件（ISR → 任务）
 *   本文件里的 s_period_queue：传递"新的闪烁周期"（任务 → 任务）
 *
 * 其中 blink_task 的写法是本示例最值得琢磨的地方，见函数内注释。
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_err.h"
#include "esp_log.h"

#include "led.h"
#include "button.h"

static const char *TAG = "main";

/* ---------- 硬件配置 ---------- */

/*
 * 外接 LED 的引脚。你的板子上板载 LED 接在 UART 线上（U0TXD/U0RXD），
 * 那是串口收发指示灯，不适合当用户 LED 用，所以这里用外接 LED。
 * 安全可用的 GPIO：1、2、4-18、21、38-42、47、48。
 * 注意 GPIO33-37 被八线 PSRAM 占用，GPIO26-32 是 Flash 总线。
 */
#define LED_GPIO 2

/*
 * 按键引脚。你板子上的 KEY2 就是 BOOT 按键，接 GPIO0，低电平有效。
 *
 * 注意 GPIO0 是 strapping 引脚：上电瞬间它的电平决定芯片是否进入下载模式。
 * 作为运行期输入完全没问题（刚才实测空闲为高电平），但**不要在复位时按住它**，
 * 否则芯片会停在 ROM 下载模式，程序不启动。
 */
#define BUTTON_GPIO 0
#define BUTTON_ACTIVE_LEVEL 0     /* 0 = 低电平有效（按键接地） */
#define BUTTON_DEBOUNCE_MS 30

/* 闪烁周期可选项，按键循环切换 */
static const uint32_t k_periods_ms[] = {1000, 500, 200, 100};
#define PERIOD_COUNT (sizeof(k_periods_ms) / sizeof(k_periods_ms[0]))

/* 任务 → 任务：传递新的闪烁周期 */
static QueueHandle_t s_period_queue = NULL;

/**
 * @brief 闪烁任务
 *
 * 关键看这里怎么"定时"。
 *
 * 直觉写法是 vTaskDelay(period) 然后循环。但那样有个问题：
 * 如果当前周期是 1000ms，按键改了周期，最坏要等 1 秒才生效。
 *
 * 这里用 xQueueReceive() 的超时参数充当延时：
 *   - 队列里没数据 → 超时返回 pdFALSE，相当于睡了 period 毫秒
 *   - 队列里有数据 → 立刻返回 pdTRUE，不用等满
 *
 * 于是"定时"和"响应事件"合并成了一次调用，周期改变**立即生效**。
 * 这是 RTOS 里非常常用的手法——用一个带超时的阻塞调用，
 * 同时实现"周期性动作"和"可被打断的等待"。
 */
static void blink_task(void *arg)
{
    (void)arg;

    uint32_t period_ms = k_periods_ms[0];
    ESP_LOGI(TAG, "闪烁任务启动，初始周期 %u ms", (unsigned)period_ms);

    while (1) {
        ESP_ERROR_CHECK(led_toggle());
        ESP_LOGI(TAG, "LED %s", led_is_on() ? "亮" : "灭");

        uint32_t new_period;
        if (xQueueReceive(s_period_queue, &new_period, pdMS_TO_TICKS(period_ms)) == pdTRUE) {
            period_ms = new_period;
            ESP_LOGI(TAG, "周期切换为 %u ms，立即生效", (unsigned)period_ms);
        }
    }
}

/**
 * @brief 按键处理任务
 *
 * 它只做一件事：等按键事件，按下就切换周期。
 * 真正的按键识别（读电平、去抖）已经在 ISR 里完成了，
 * 这里拿到的都是"已经确认过的事件"。
 */
static void button_task(void *arg)
{
    (void)arg;

    QueueHandle_t evq = button_get_event_queue();
    if (evq == NULL) {
        ESP_LOGE(TAG, "按键事件队列为空，任务退出");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "按键任务启动，按下 BOOT 键切换闪烁周期");

    int idx = 0;
    while (1) {
        button_event_t ev;
        /* portMAX_DELAY = 无限等待。本任务平时完全休眠，不占 CPU，
         * 只有按键事件到来时才会被唤醒。这是事件驱动任务的标准写法。 */
        if (xQueueReceive(evq, &ev, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        /* 只处理按下，松开事件忽略 */
        if (ev != BUTTON_EVENT_PRESSED) {
            continue;
        }

        idx = (idx + 1) % PERIOD_COUNT;
        uint32_t p = k_periods_ms[idx];
        ESP_LOGI(TAG, "按键按下 → 请求切换到 %u ms", (unsigned)p);

        /* 发给闪烁任务。超时传 0：队列满就丢弃，绝不在这里阻塞 */
        xQueueSend(s_period_queue, &p, 0);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "app_main 开始");

    ESP_ERROR_CHECK(led_init(LED_GPIO));

    s_period_queue = xQueueCreate(4, sizeof(uint32_t));
    if (s_period_queue == NULL) {
        ESP_LOGE(TAG, "创建周期队列失败");
        return;
    }

    const button_config_t btn_cfg = {
        .gpio_num = BUTTON_GPIO,
        .active_level = BUTTON_ACTIVE_LEVEL,
        .debounce_ms = BUTTON_DEBOUNCE_MS,
    };
    ESP_ERROR_CHECK(button_init(&btn_cfg));

    /*
     * 创建两个任务。优先级给相同的 5——它们互不抢占，
     * 靠队列阻塞自然协作。如果按键要求响应更快，可以把 button_task 提到 6。
     */
    xTaskCreate(blink_task,  "blink",  2048, NULL, 5, NULL);
    xTaskCreate(button_task, "button", 3072, NULL, 5, NULL);

    /* app_main 返回，main 任务结束，上面两个任务继续运行 */
    ESP_LOGI(TAG, "app_main 返回，blink_task 与 button_task 继续运行");
}
