/*
 * monitor_task.c —— 系统监控任务（文档 §38）
 *
 * 每 MONITOR_INTERVAL_MS（默认 10 秒）打两行：
 *
 *   堆 / 最小堆 / RSSI / MQTT 状态 / 采样计数 / 复位原因
 *   各任务的栈水位
 *
 * ── 为什么需要它 ────────────────────────────────────────────────
 *
 * 嵌入式系统最难查的三类问题是：死机、重启、内存不足。它们的共同点是
 * **发生的时候你不在现场**——等你发现设备不对劲时，现场已经没了。
 * 串口日志是唯一的现场记录，所以必须在"还正常的时候"就周期性留下
 * 这些数字，事后才能对着时间轴找出异常是什么时候开始的。
 *
 * 具体到这个阶段，它回答三个问题：
 *   1. 栈够不够？   —— 各任务的 uxTaskGetStackHighWaterMark
 *   2. 内存漏没漏？ —— Free Heap 与**历史最低** Free Heap 的差距
 *   3. 谁在丢数据？ —— 采样计数与两条队列的丢弃计数
 *
 * ── 为什么优先级低于所有业务任务 ────────────────────────────────
 *
 * 监控是"看"，不该和"干"抢资源。给它最低优先级意味着：只要有任何
 * 业务任务就绪，它就等着。它的周期是 10 秒量级，晚几百毫秒完全
 * 不影响判断。反过来，如果给它高优先级，它每次醒来都会打断采样、
 * 打印、上报——为了一个观测动作牺牲业务实时性，是本末倒置。
 *
 * ── 为什么栈水位要在这里统一打，而不是各任务自己打 ──────────────
 *
 * 各任务本来就在自己循环里调 app_report_stack()，那能回答"我这个任务
 * 够不够"。但调栈大小时需要的是**横向对比**：哪个任务最吃栈、
 * 余量最少的那个还剩多少。散在各任务日志里的数字要人工对齐时间戳
 * 才能比较，而且它们上报的周期不同，很难凑成同一时刻的快照。
 *
 * 所以这里用 uxTaskGetStackHighWaterMark(handle) 统一采一遍。
 * 句柄由 app.c 在 xTaskCreate 时存进 app_ctx_t::task_handles。
 *
 * 注意：任务的**名称 / 栈大小 / 优先级不在这里**——统一放在 app.c。
 */
#include "app_internal.h"

#include <stdio.h>
#include <string.h>

#include "freertos/task.h"

#include "esp_log.h"
#include "esp_system.h"

#include "mqtt_mgr.h"
#include "wifi_mgr.h"

static const char *TAG = "monitor";

/*
 * 复位原因转成中文短标签。
 *
 * 为什么不直接打枚举值：排查时最常问的是"它是自己重启了还是被人
 * 断电了"。PANIC / TASK_WDT / BROWNOUT 三个值分别指向三种完全不同的
 * 原因（代码 bug / 任务卡死 / 供电不足），而数字形式需要每次都去翻
 * 头文件。字符串一眼就能看出方向。
 */
static const char *reset_reason_str(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON:   return "上电";
    case ESP_RST_EXT:       return "外部复位";
    case ESP_RST_SW:        return "软件复位";
    case ESP_RST_PANIC:     return "异常(panic)";
    case ESP_RST_INT_WDT:   return "中断看门狗";
    case ESP_RST_TASK_WDT:  return "任务看门狗";
    case ESP_RST_WDT:       return "其它看门狗";
    case ESP_RST_DEEPSLEEP: return "深睡唤醒";
    case ESP_RST_BROWNOUT:  return "掉电(brownout)";
    case ESP_RST_SDIO:      return "SDIO";
    case ESP_RST_UNKNOWN:   /* 落到 default */
    default:                return "未知";
    }
}

/**
 * @brief 打一行所有任务的栈水位
 *
 * 输出形如：
 *   栈水位(字节) blink=1400 button=2100 rgb=1800 sensor=1200 console=2400 mqtt=2600 monitor=2800
 *
 * 怎么读这些数：**它越接近 0 越危险**，经验判据是余量留 30% 以上。
 * 单位是字节（ESP-IDF 把 portSTACK_TYPE 定义成了 uint8_t，
 * 不要乘 sizeof(StackType_t) 去"换算"），详见 app_internal.h。
 */
static void report_task_stacks(const app_ctx_t *ctx)
{
    char line[256];
    int  off = 0;

    /*
     * 手工拼串而不是多次 ESP_LOGI：一行打完，串口上不会被其它任务的
     * 日志插进来。用 snprintf 并推进 off，边界始终受 sizeof 保护。
     */
    off += snprintf(line + off, sizeof(line) - (size_t)off, "栈水位(字节)");

    for (int i = 0; i < APP_TASK_COUNT; i++) {
        const TaskHandle_t h = ctx->task_handles[i];
        if (h == NULL) {
            /* 任务没建起来（app_start 会先返回错误，理论上到不了这里） */
            continue;
        }

        /*
         * uxTaskGetStackHighWaterMark() 返回该任务**历史最低**的剩余栈，
         * 不是当前剩余——这正是我们要的：栈的峰值用量才是决定
         * 大小是否足够的依据，瞬时值可能恰好落在低谷。
         *
         * 传别人的句柄是允许的；传 NULL 才是"我自己"。
         */
        off += snprintf(line + off, sizeof(line) - (size_t)off, " %s=%u",
                        pcTaskGetName(h),
                        (unsigned)uxTaskGetStackHighWaterMark(h));

        if ((size_t)off >= sizeof(line)) {
            /* 缓冲满了就停下，避免越界。任务数量远不到这个程度，
             * 但边界检查是免费的，留着比不留好 */
            break;
        }
    }

    ESP_LOGI(TAG, "%s", line);
}

void app_monitor_task(void *arg)
{
    const app_ctx_t *ctx = (const app_ctx_t *)arg;

    /*
     * 复位原因是个常量，任务启动时读一次就够。
     *
     * 为什么不每轮都读：esp_reset_reason() 返回的是**本次启动**的原因，
     * 运行期间不会变。每轮读一次除了浪费没有别的效果。
     *
     * 它的价值在于：如果设备在反复重启，串口里会一遍遍出现这一行，
     * 而"上电"和"任务看门狗"这两种原因指向完全不同的排查方向。
     */
    const char *reset_str = reset_reason_str(esp_reset_reason());

    /*
     * 周期兜底。0 会让 vTaskDelay(0) 变成不阻塞的紧循环——
     * 与 sensor_task 里那个同样的防御，理由也一样：
     * 静默接受一个非法值比明确告警更糟。
     */
    uint32_t interval_ms = ctx->monitor_interval_ms;
    if (interval_ms == 0) {
        ESP_LOGW(TAG, "监控周期为 0 非法（会变成无延时的死循环），改用 10000 ms");
        interval_ms = 10000;
    }

    ESP_LOGI(TAG, "监控任务启动，周期 %u ms，本次复位原因：%s",
             (unsigned)interval_ms, reset_str);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(interval_ms));

        /*
         * 内存。
         *
         * 关键看的是**两个数的差**，而不是绝对值：
         *   Free Heap       —— 此刻还剩多少
         *   Min Free Heap   —— 开机以来的历史最低值
         * 如果 Min 明显低于当前 Free，说明曾经有过一段内存紧张的时刻
         * （通常是某个高峰期的临时分配）。如果 Free 在一段时间里
         * 持续单调下降、且 Min 不断被刷新，那就是泄漏。
         */
        const uint32_t free_heap = esp_get_free_heap_size();
        const uint32_t min_heap  = esp_get_minimum_free_heap_size();

        /*
         * WiFi 信号强度。未连接时输出 "-" 而不是某个数字——
         * 理由同报文里的 rssi：0 dBm 是合法且极强的读数，
         * 不能用它表示"没有"。
         */
        char rssi_str[8];
        int8_t rssi = 0;
        if (wifi_mgr_get_rssi(&rssi) == ESP_OK) {
            snprintf(rssi_str, sizeof(rssi_str), "%d", (int)rssi);
        } else {
            strcpy(rssi_str, "-");
        }

        app_sensor_stats_t st;
        app_sensor_get_stats(&st);

        /*
         * 一行打完所有状态量。分多行打会被其它任务的日志穿插，
         * 事后要把它们拼回同一时刻反而更费劲。
         */
        ESP_LOGI(TAG,
                 "堆 %u/%u 字节 | WiFi %s dBm | MQTT %s | "
                 "采样 ok=%u fail=%u 丢弃(串口=%u mqtt=%u) | 复位 %s",
                 (unsigned)free_heap, (unsigned)min_heap,
                 rssi_str, mqtt_mgr_state_str(),
                 (unsigned)st.reads_ok, (unsigned)st.reads_failed,
                 (unsigned)st.dropped_console, (unsigned)st.dropped_mqtt,
                 reset_str);

        report_task_stacks(ctx);
    }
}
