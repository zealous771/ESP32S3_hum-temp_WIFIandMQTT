/*
 * led.c —— led 组件的实现
 *
 * 这个文件演示了 ESP-IDF 组件的几个惯用法，都和 HAL 有对应关系：
 *
 *   1. 错误处理：每个对外接口返回 esp_err_t，而不是 void。
 *      HAL 里 HAL_GPIO_Init() 也是返回 void 的，出错只能靠现象猜；
 *      ESP-IDF 把错误码一路传出来，调用方用 ESP_ERROR_CHECK() 决定是否当场崩溃。
 *
 *   2. 日志：用 ESP_LOGI/W/E 而不是 printf。
 *      这些宏会自动带上模块标签、日志级别和毫秒时间戳，
 *      而且日志级别可以在 menuconfig 里按模块调整，不用改代码。
 *
 *   3. 状态封装：用文件作用域的 static 变量保存状态。
 *      外部拿不到 s_gpio_num，只能通过接口操作——避免了 HAL 里
 *      到处可见的全局句柄被随手改坏。
 */
#include "led.h"

#include "driver/gpio.h"
#include "esp_log.h"

/* 日志标签。约定用组件名，方便在串口输出里过滤。 */
static const char *TAG = "led";

/* 未初始化的哨兵值。用 -1 而不是 0，因为 GPIO0 是合法引脚。 */
#define LED_GPIO_UNINIT (-1)

static int s_gpio_num = LED_GPIO_UNINIT;
static int s_active_level = 1;   /* 点亮时的电平，由 led_init 按配置填入 */
static bool s_is_on = false;

/*
 * 输出"亮"或"灭"对应的电平。
 *
 * 因为 active_level 只可能是 0 或 1，灭的电平就是它的取反。
 * 把换算收在这一个函数里，led_on / led_off / led_init 都不用关心
 * 具体接法——这正是把"有效电平"抽出来的意义。
 */
static void set_output(bool on)
{
    gpio_set_level(s_gpio_num, on ? s_active_level : !s_active_level);
    s_is_on = on;
}

/* 检查是否已初始化。所有对外接口都先过这一关，避免用未初始化的引脚。 */
static bool is_initialized(void)
{
    return s_gpio_num != LED_GPIO_UNINIT;
}

esp_err_t led_init(const led_config_t *config)
{
    if (config == NULL || config->gpio_num < 0) {
        ESP_LOGE(TAG, "配置非法");
        return ESP_ERR_INVALID_ARG;
    }
    if (config->active_level != 0 && config->active_level != 1) {
        ESP_LOGE(TAG, "active_level 只能是 0 或 1，收到 %d", config->active_level);
        return ESP_ERR_INVALID_ARG;
    }
    if (is_initialized()) {
        ESP_LOGE(TAG, "已经初始化过了");
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * gpio_reset_pin() 会把引脚复位到默认状态：关闭上下拉、复位为输入。
     * 这一步很重要——ESP32 上电后很多引脚处于不确定状态，
     * 或者被 ROM bootloader 用过（比如 strapping 引脚）。
     *
     * 注意它不会检查引脚是否被别的外设占用。如果你选的引脚
     * 正在被 USB-JTAG、Flash、PSRAM 或 UART0 使用，这里不会报错，
     * 但行为会异常。选引脚前先确认它没被占用（见 main/Kconfig.projbuild 的说明）。
     */
    gpio_reset_pin(config->gpio_num);
    gpio_set_direction(config->gpio_num, GPIO_MODE_OUTPUT);

    s_gpio_num = config->gpio_num;
    s_active_level = config->active_level;

    /* 初始化后默认熄灭，避免上电就亮着 */
    set_output(false);

    ESP_LOGI(TAG, "LED 初始化完成：GPIO%d，%s",
             config->gpio_num,
             config->active_level == 1 ? "高电平点亮（拉电流）" : "低电平点亮（灌电流）");
    return ESP_OK;
}

esp_err_t led_on(void)
{
    if (!is_initialized()) {
        ESP_LOGE(TAG, "尚未初始化，请先调用 led_init()");
        return ESP_ERR_INVALID_STATE;
    }

    set_output(true);
    return ESP_OK;
}

esp_err_t led_off(void)
{
    if (!is_initialized()) {
        ESP_LOGE(TAG, "尚未初始化，请先调用 led_init()");
        return ESP_ERR_INVALID_STATE;
    }

    set_output(false);
    return ESP_OK;
}

esp_err_t led_toggle(void)
{
    if (!is_initialized()) {
        ESP_LOGE(TAG, "尚未初始化，请先调用 led_init()");
        return ESP_ERR_INVALID_STATE;
    }

    /* 复用 led_on/led_off，避免在这里重复写 gpio_set_level */
    return s_is_on ? led_off() : led_on();
}

bool led_is_on(void)
{
    return s_is_on;
}
