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

#define RGB_TASK_NAME   "rgb"
#define RGB_TASK_STACK  2560
#define RGB_TASK_PRIO   5

/* 色相每步走多少度、每步之间停多久 */
#define HUE_STEP_DEG    3
#define HUE_STEP_MS     60

void app_rgb_task(void *arg)
{
    /* 本任务只用固定的第 0 颗灯，不需要上下文里的周期表 */
    (void)arg;

    uint32_t hue = 0;
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
        vTaskDelay(pdMS_TO_TICKS(HUE_STEP_MS));
    }
}
