/*
 * button.c —— 按键组件实现
 *
 * 核心设计：ISR 与任务的分工。
 *
 *   ISR（button_isr_handler）
 *     ├─ 读引脚电平
 *     ├─ 用 tick 计数做时间去抖（几纳秒，纯算术）
 *     ├─ 把事件塞进队列
 *     └─ 立刻返回
 *
 *   任务（由调用方创建）
 *     └─ xQueueReceive() 取事件，慢慢做业务
 *
 * 关于去抖为什么放在 ISR 里而不是任务里：
 *   机械按键按下瞬间会产生几十次通断（抖动）。如果 ISR 把每次边沿都塞进队列，
 *   队列很快会被垃圾事件填满，任务还得再过滤一遍。
 *   在 ISR 里用"距上次事件不足 N 毫秒就丢弃"这个纯算术判断，
 *   成本只有几十条指令，却能把 99% 的抖动挡在队列之外。
 *   这是个很典型的权衡——用极小的中断时间换取大幅减少的队列流量。
 */
#include "button.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_attr.h"

static const char *TAG = "button";

/* 未初始化的哨兵。用 -1 而不是 0，因为 GPIO0 是合法引脚。 */
#define BUTTON_GPIO_UNINIT (-1)

static int s_gpio_num = BUTTON_GPIO_UNINIT;
static int s_active_level = 0;
static TickType_t s_debounce_ticks = 0;
static TickType_t s_last_event_tick = 0;
static QueueHandle_t s_event_queue = NULL;

/*
 * 中断服务程序（ISR）。
 *
 * 注意这里没有标 IRAM_ATTR，因为下面安装中断时用的是默认标志（不带 ESP_INTR_FLAG_IRAM）。
 * 这两者是配套的：
 *   - 若希望 ISR 在 Flash 被擦写（Cache 禁用）期间仍能响应，需要
 *     gpio_install_isr_service(ESP_INTR_FLAG_IRAM) 且给本函数加 IRAM_ATTR，
 *     同时保证它调用到的所有函数也都在 IRAM 里。
 *   - 本组件用默认标志，ISR 留在 Flash 中，靠 Cache 执行——对按键这种
 *     慢速输入完全够用，也避免了"漏标一个函数就崩溃"的风险。
 * 这就是第一课里讲的 IRAM 取舍在真实代码里的体现。
 */
static void button_isr_handler(void *arg)
{
    /* 时间去抖：距上次事件太近的直接丢弃，不往队列里塞 */
    TickType_t now = xTaskGetTickCountFromISR();
    if ((now - s_last_event_tick) < s_debounce_ticks) {
        return;
    }
    s_last_event_tick = now;

    /* 读当前电平，判断是按下还是松开 */
    int level = gpio_get_level(s_gpio_num);
    button_event_t ev = (level == s_active_level) ? BUTTON_EVENT_PRESSED : BUTTON_EVENT_RELEASED;

    /*
     * 往队列发事件。必须用带 FromISR 后缀的版本——
     * 普通版 xQueueSend() 内部可能会阻塞，在中断上下文里调用会直接崩。
     */
    BaseType_t hp_task_woken = pdFALSE;
    xQueueSendFromISR(s_event_queue, &ev, &hp_task_woken);

    /*
     * 如果刚才的发送唤醒了一个优先级更高的任务，需要请求一次上下文切换。
     * 不写这句也能跑，但被唤醒的高优先级任务要等到下一个 tick 才能运行，
     * 响应会变慢。这是 ISR 里的标准收尾动作。
     */
    if (hp_task_woken) {
        portYIELD_FROM_ISR();
    }
}

esp_err_t button_init(const button_config_t *config)
{
    if (config == NULL || config->gpio_num < 0) {
        ESP_LOGE(TAG, "配置非法");
        return ESP_ERR_INVALID_ARG;
    }
    if (s_gpio_num != BUTTON_GPIO_UNINIT) {
        ESP_LOGE(TAG, "已经初始化过了");
        return ESP_ERR_INVALID_STATE;
    }

    s_gpio_num = config->gpio_num;
    s_active_level = config->active_level;
    s_debounce_ticks = pdMS_TO_TICKS(config->debounce_ms);
    s_last_event_tick = 0;

    /* 配置 GPIO 为输入。
     * 低电平有效（按键接地）就开内部上拉；高电平有效就开内部下拉。
     * 很多板子外部已经有上拉电阻，这里的内部上拉只是多一层保险。 */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << config->gpio_num),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = (config->active_level == 0) ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = (config->active_level == 0) ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE,
        /* 双边沿触发：按下和松开都要捕获 */
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config 失败: %s", esp_err_to_name(ret));
        s_gpio_num = BUTTON_GPIO_UNINIT;
        return ret;
    }

    /* 创建事件队列，深度 10 足够——去抖已经把大部分事件挡掉了 */
    s_event_queue = xQueueCreate(10, sizeof(button_event_t));
    if (s_event_queue == NULL) {
        ESP_LOGE(TAG, "创建队列失败");
        s_gpio_num = BUTTON_GPIO_UNINIT;
        return ESP_ERR_NO_MEM;
    }

    /*
     * 安装 GPIO 中断服务。这个函数全局只需调用一次，
     * 多个按键共用同一个服务。如果别的组件已经装过，会返回 ESP_ERR_INVALID_STATE，
     * 这属于正常情况，不算错误。
     */
    ret = gpio_install_isr_service(0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "安装 ISR 服务失败: %s", esp_err_to_name(ret));
        vQueueDelete(s_event_queue);
        s_event_queue = NULL;
        s_gpio_num = BUTTON_GPIO_UNINIT;
        return ret;
    }

    /* 把本引脚的 ISR 挂上去 */
    ret = gpio_isr_handler_add(config->gpio_num, button_isr_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "注册 ISR 失败: %s", esp_err_to_name(ret));
        vQueueDelete(s_event_queue);
        s_event_queue = NULL;
        s_gpio_num = BUTTON_GPIO_UNINIT;
        return ret;
    }

    ESP_LOGI(TAG, "按键初始化完成：GPIO%d，%s有效，去抖 %u ms",
             config->gpio_num,
             config->active_level == 0 ? "低电平" : "高电平",
             (unsigned)config->debounce_ms);
    return ESP_OK;
}

QueueHandle_t button_get_event_queue(void)//用于把句柄分享给其他组件进行任务的操作。
{
    return s_event_queue;
}
