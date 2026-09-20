/*
 * blink_task.c —— 闪烁任务
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
#include "app_internal.h"

#include "esp_err.h"
#include "esp_log.h"

#include "led.h"

static const char *TAG = "blink";

/*
 * 每多少轮上报一次栈水位。
 * 这里按"轮数"而不是时间计——周期本身是可变的（按键会改），
 * 用轮数更简单，代价是上报间隔会随周期变化。
 */
#define STACK_REPORT_EVERY_CYCLES 10

void app_blink_task(void *arg)
{
    /* pvParameters 传进来的是 &s_ctx，在这里取回自己的上下文 */
    const app_ctx_t *ctx = (const app_ctx_t *)arg;

    uint32_t period_ms = ctx->periods_ms[0];
    uint32_t cycles = 0;
    ESP_LOGI(TAG, "闪烁任务启动，初始周期 %u ms", (unsigned)period_ms);

    while (1) {
        ESP_ERROR_CHECK(led_toggle());
        ESP_LOGI(TAG, "LED %s", led_is_on() ? "亮" : "灭");

        uint32_t new_period;
        if (xQueueReceive(ctx->period_queue, &new_period, pdMS_TO_TICKS(period_ms)) == pdTRUE) {
            period_ms = new_period;
            ESP_LOGI(TAG, "周期切换为 %u ms，立即生效", (unsigned)period_ms);
        }

        if (++cycles % STACK_REPORT_EVERY_CYCLES == 0) {
            app_report_stack(TAG);
        }
    }
}
