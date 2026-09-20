/*
 * rgb_task.c —— 板载可寻址 RGB 的演示任务
 *
 * 让灯珠缓慢走一圈色相（彩虹效果），演示 rgb_led 组件的用法。
 *
 * 为什么这里用 HSV 而不是 RGB？
 *
 *   想做"颜色缓慢变化"，在 RGB 空间里很别扭——从红走到绿，
 *   你得同时把 R 从 255 降到 0、G 从 0 升到 255，中间还会经过
 *   一段发暗的黄。因为 RGB 的三个分量是**耦合**的：
 *   它们描述的是"三盏小灯各开多亮"，不是"颜色本身"。
 *
 *   HSV 把颜色拆成三个正交的维度：
 *     H 色相   —— 是什么颜色（红 → 橙 → 黄 → 绿 → 青 → 蓝 → 紫）
 *     S 饱和度 —— 有多浓（0 是白）
 *     V 明度   —— 有多亮（0 是黑）
 *
 *   想要彩虹效果，只需要让 H 从 0 走到 360，S 和 V 固定不动。
 *   这就是为什么调颜色时 HSV 比 RGB 直观——改一个维度只影响一个属性。
 */
#include "app_internal.h"

#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"

#include "rgb_led.h"

static const char *TAG = "rgb";

/*
 * 色相每步走多少度、每步之间停多久。
 *
 * 注意：任务的**名称 / 栈大小 / 优先级不在这里**——它们统一放在 app.c。
 * 一个任务"占多少资源"和"干什么活"分开放，调整时不用翻遍所有文件。
 * （这三个宏曾经在本文件里重复定义过一份，但从未被使用，已删除。）
 */
#define HUE_STEP_DEG    3
#define HUE_STEP_MS     60

/* 每多少轮上报一次栈水位。60ms 一轮，约 5 秒报一次 */
#define STACK_REPORT_EVERY_CYCLES 83

void app_rgb_task(void *arg)
{
    /* 本任务只用固定的第 0 颗灯，不需要上下文里的周期表 */
    (void)arg;

    uint32_t hue = 0;
    uint32_t cycles = 0;
    ESP_LOGI(TAG, "RGB 任务启动，色相循环：H 每步 +%d 度，间隔 %d ms",
             HUE_STEP_DEG, HUE_STEP_MS);

    while (1) {
        /*
         * 饱和度给满，明度压到 12%。
         * 全亮度的 RGB 直看很刺眼，而且板载灯珠离眼睛近。
         * 明度是百分比，组件内部会换算成驱动要的 0~255。
         */
        ESP_ERROR_CHECK(rgb_led_set_hsv(0, hue, 100, 12));

        hue = (hue + HUE_STEP_DEG) % 360;

        if (++cycles % STACK_REPORT_EVERY_CYCLES == 0) {
            app_report_stack(TAG);
        }

        vTaskDelay(pdMS_TO_TICKS(HUE_STEP_MS));
    }
}
