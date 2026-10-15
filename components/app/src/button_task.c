/*
 * button_task.c —— 按键处理任务
 *
 * 它只做一件事：等按键事件，按下就请求切换周期。
 *
 * 真正的按键识别（读电平、去抖）已经在 button 组件的 ISR 里完成了，
 * 这里拿到的都是"已经确认过的事件"。
 *
 * 注意本文件涉及两条不同的队列，别混淆：
 *   1. button 组件的 s_event_queue —— ISR → 本任务（传递按键事件）
 *   2. ctx->period_queue           —— 本任务 → blink_task（传递新周期）
 */
#include "app_internal.h"

#include "freertos/task.h"

#include "esp_log.h"

#include "button.h"
#include "settings.h"

static const char *TAG = "button_task";

void app_button_task(void *arg)
{
    const app_ctx_t *ctx = (const app_ctx_t *)arg;

    QueueHandle_t evq = button_get_event_queue();
    if (evq == NULL) {
        ESP_LOGE(TAG, "按键事件队列为空，任务退出");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "按键任务启动，按下 BOOT 键切换闪烁周期");

    /*
     * 从"上次保存的那一档"开始，而不是永远从第 0 档。
     * 这样按键的循环顺序与开机时的实际周期是对上的——
     * 否则开机后第一次按键会跳到一个和当前周期不相邻的档位。
     */
    size_t idx = ctx->initial_period_index;
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

        idx = (idx + 1) % ctx->period_count;
        uint32_t p = ctx->periods_ms[idx];
        ESP_LOGI(TAG, "按键按下 → 请求切换到 %u ms", (unsigned)p);

        /* 发给闪烁任务。超时传 0：队列满就丢弃，绝不在这里阻塞 */
        xQueueSend(ctx->period_queue, &p, 0);

        /*
         * 存进 NVS，下次开机接着用这一档。
         *
         * 注意这里**故意不用 ESP_ERROR_CHECK**：保存失败只影响
         * "下次开机记不记得住"，周期切换本身已经生效了。
         * 为一个非关键功能的失败让整个系统 abort 是不划算的。
         *
         * 这是嵌入式里常见的一个判断：**区分"功能失败"和
         * "辅助功能失败"**——前者要立刻暴露，后者记一笔日志继续跑。
         */
        esp_err_t ret = settings_set_period_ms(p);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "周期保存失败（%s），本次切换仍然生效，但重启后会丢失",
                     esp_err_to_name(ret));
        }

        /*
         * 上报栈水位。本任务平时只是阻塞在队列上，栈用得很少，
         * 所以要在"刚处理完一次事件"之后报——那才是它栈最深的时候。
         */
        app_report_stack(TAG);
    }
}
