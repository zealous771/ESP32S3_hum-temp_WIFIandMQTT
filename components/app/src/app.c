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

#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"

#include "button.h"
#include "led.h"

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

static esp_err_t init_peripherals(const app_config_t *config)
{
    esp_err_t ret = led_init(config->led_gpio);
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
    return ESP_OK;
}

static esp_err_t create_tasks(void)
{
    /*
     * 两个任务的优先级都给 5——它们互不抢占，靠队列阻塞自然协作。
     * 如果按键要求响应更快，可以把 button_task 提到 6。
     *
     * pvParameters 传 &s_ctx：任务通过它拿到周期表和队列，
     * 不需要访问任何全局变量。
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
    return ESP_OK;
}

esp_err_t app_start(const app_config_t *config)
{
    if (config == NULL || config->periods_ms == NULL || config->period_count == 0) {
        ESP_LOGE(TAG, "配置非法");
        return ESP_ERR_INVALID_ARG;
    }

    s_ctx.periods_ms   = config->periods_ms;
    s_ctx.period_count = config->period_count;

    esp_err_t ret = init_peripherals(config);
    if (ret != ESP_OK) {
        return ret;
    }

    /* 深度 4 够用：只有按键按下时写一次，而 blink_task 每次都读走 */
    s_ctx.period_queue = xQueueCreate(4, sizeof(uint32_t));
    if (s_ctx.period_queue == NULL) {
        ESP_LOGE(TAG, "创建周期队列失败");
        return ESP_ERR_NO_MEM;
    }

    ret = create_tasks();
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_LOGI(TAG, "应用启动完成");
    return ESP_OK;
}
