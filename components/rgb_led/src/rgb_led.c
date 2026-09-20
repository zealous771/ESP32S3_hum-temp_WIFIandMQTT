/*
 * rgb_led.c —— 可寻址 RGB 的实现
 *
 * 本文件本身很薄，绝大部分工作由 espressif/led_strip 组件完成。
 * 值得看的是三件事：
 *   1. 驱动句柄怎么管理（static，不给外部）
 *   2. RMT 的配置项分别意味着什么
 *   3. HSV 到驱动接口的单位换算
 */
#include "rgb_led.h"

#include <stddef.h>

#include "esp_log.h"

#include "led_strip.h"

static const char *TAG = "rgb_led";

/*
 * 驱动句柄。和 led / button 组件一样，状态藏在组件内部，
 * 外部只通过函数操作，拿不到句柄也就改不坏。
 */
static led_strip_handle_t s_strip = NULL;
static uint32_t s_led_count = 0;

/* HSV 的饱和度/明度本组件对外用 0~100，驱动用 0~255 */
#define HSV_PERCENT_TO_255(p) ((uint8_t)(((p) > 100 ? 100 : (p)) * 255 / 100))

esp_err_t rgb_led_init(int gpio_num, uint32_t led_count)
{
    if (gpio_num < 0 || led_count == 0) {
        ESP_LOGE(TAG, "参数非法：gpio=%d count=%u", gpio_num, (unsigned)led_count);
        return ESP_ERR_INVALID_ARG;
    }
    if (s_strip != NULL) {
        ESP_LOGW(TAG, "已经初始化过了");
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * 灯带本身的配置。两个字段值得单独说：
     *
     *   led_model  —— 不同型号的灯珠时序参数不同（WS2812 / SK6812 / WS2811 …），
     *                 驱动按型号查表决定 0 码和 1 码各占多少纳秒。
     *                 写错型号的后果是颜色错乱或完全不亮。
     *
     *   color_component_format —— 每颗灯内部三个通道的**排列顺序**。
     *                 WS2812 是 GRB 而不是 RGB——它把绿色放在最前面。
     *                 所以就算你按 RGB 顺序填数据，看到的红蓝也是反的。
     *                 这是新手最常踩的坑：明明代码写着"红"，
     *                 灯却是绿的。
     */
    led_strip_config_t strip_config = {
        .strip_gpio_num = gpio_num,
        .max_leds = led_count,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags.invert_out = false,
    };

    /*
     * RMT 后端的配置。
     *
     *   resolution_hz —— RMT 计数器的时钟频率，决定了波形的**时间分辨率**。
     *                    10 MHz 意味着一个计数单位是 100 ns。
     *                    WS2812 的 0 码高电平约 400 ns、1 码约 800 ns，
     *                    用 100 ns 的刻度去表示刚好够用（4 格 / 8 格）。
     *                    频率调低会让波形粗糙、时序超出容差；
     *                    调高则每个符号占用的内存变大。
     *
     *   with_dma —— 是否用 DMA 搬运波形数据。
     *                    灯珠少的时候没必要（本板只有 1 颗），
     *                    长灯带（几百颗）打开可以避免占用 CPU。
     */
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000,   /* 10 MHz → 100 ns / tick */
        .flags.with_dma = false,
    };

    esp_err_t ret = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "创建 LED 设备失败: %s", esp_err_to_name(ret));
        s_strip = NULL;
        return ret;
    }

    s_led_count = led_count;

    /* 上电时灯珠里可能是随机内容，先清掉 */
    led_strip_clear(s_strip);

    ESP_LOGI(TAG, "初始化完成：GPIO%d，%u 颗灯，WS2812 / GRB，RMT 10MHz",
             gpio_num, (unsigned)led_count);
    return ESP_OK;
}

esp_err_t rgb_led_set(uint32_t index, uint8_t r, uint8_t g, uint8_t b)
{
    if (s_strip == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (index >= s_led_count) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 写进驱动的内存缓冲区，此时灯还没变 */
    esp_err_t ret = led_strip_set_pixel(s_strip, index, r, g, b);
    if (ret != ESP_OK) {
        return ret;
    }

    /*
     * 推送到灯带。这一步才会在数据线上产生波形，
     * 并在末尾补一个 >50us 的低电平（复位脉冲），灯珠收到后才锁存显示。
     * 所以"设置像素"和"刷新"是两回事——设了不刷新，灯不会变。
     */
    return led_strip_refresh(s_strip);
}

esp_err_t rgb_led_set_hsv(uint32_t index, uint32_t h, uint32_t s, uint32_t v)
{
    if (s_strip == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (index >= s_led_count) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = led_strip_set_pixel_hsv(s_strip, index,
                                           (uint16_t)(h % 360),
                                           HSV_PERCENT_TO_255(s),
                                           HSV_PERCENT_TO_255(v));
    if (ret != ESP_OK) {
        return ret;
    }
    return led_strip_refresh(s_strip);
}

esp_err_t rgb_led_clear(void)
{
    if (s_strip == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return led_strip_clear(s_strip);
}

uint32_t rgb_led_count(void)
{
    return s_led_count;
}
