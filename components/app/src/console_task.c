/*
 * console_task.c —— 串口消费者
 *
 * 它是采样队列的**第一个消费者**，职责只有一件：从队列取出记录，
 * 格式化成一行 JSON 打到串口。
 *
 * 为什么要有它？因为它是"不依赖任何外部服务的观测点"。
 *
 *   接 MQTT 之后，mqtt_task 会成为真正的消费者。但 mqtt_task 的输出要
 *   经过 broker 才能看到——而 broker 没起、Windows 防火墙拦住 1883、
 *   Mosquitto 只监听 127.0.0.1、SSID 没填，任何一个环节出问题，
 *   你都会"什么都看不到"，并且无法判断到底是采样停了、队列断了，
 *   还是仅仅网络不通。
 *
 *   console_task 不看网络。它只要能打印，就证明
 *   「传感器 → 采样任务 → 队列」这一段是好的。
 *   排错时它把"本地链路"和"网络链路"彻底分开——
 *   所以接上 MQTT 之后**不应该删掉它**，它是常驻的诊断通道。
 *
 * 阻塞策略与生产者正好相反，这是有意的：
 *
 *   生产者 xQueueSend(..., 0)              队列满就丢，保住采样节奏
 *   消费者 xQueueReceive(..., portMAX_DELAY) 队列空就无限等，不空转
 *
 *   两边各取所需：一个要"绝不等待"，一个要"绝不空转"。
 *   消费者若改用"0 超时轮询"，会在队列空时以 CPU 全速空跑——
 *   它和其它任务同为优先级 5，空转虽然会让出 CPU，但白白消耗调度，
 *   而且忙等期间的功耗和日志噪声都是纯浪费。
 *   （阻塞等待还有个好处：队列里有数据时任务被立刻唤醒，
 *     延迟由内核调度决定，比轮询周期的平均值小得多。）
 *
 * 注意：任务的**名称 / 栈大小 / 优先级不在这里**——统一放在 app.c。
 */
#include "app_internal.h"
#include "data_model.h"
#include "telemetry_json.h"

#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"

#include "wifi_mgr.h"

static const char *TAG = "console";

/*
 * 每多少条上报一次栈水位。
 * 本任务是整个 app 里最吃栈的那个——它每轮都要走一遍
 * snprintf + ESP_LOGI 的格式化路径，而格式化是最费栈的操作
 * （snprintf 内部要用上百字节的转换缓冲，日志后端还会再格式化一次，
 * 中断也可能在此期间嵌套）。所以它尤其值得盯着。
 */
#define STACK_REPORT_EVERY_CYCLES 5

void app_console_task(void *arg)
{
    const app_ctx_t *ctx = (const app_ctx_t *)arg;

    ESP_LOGI(TAG, "串口消费者启动，等待采样队列");

    sensor_data_t d;
    uint32_t      cycles = 0;

    while (1) {
        /*
         * portMAX_DELAY = 一直等到有数据为止。
         * 返回值理论上恒为 pdTRUE（这条队列不会有别的消费者来抢），
         * 但既然 API 允许返回失败，就按 API 的契约判一下——
         * 万一日后有人加了个带超时的分支，这里不会静默读出脏数据。
         */
        if (xQueueReceive(ctx->sensor_queue, &d, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        /*
         * 缓冲给 192 字节。当前报文最长约 110 字节
         * （device_id 31 + ts 19 位 + seq 10 位 + 各字段 + 语法字符），
         * 余量留足，免得将来加一个字段（比如 rssi）就要回头改这里。
         */
        char json[192];
        const int n = app_format_reading_json(json, sizeof(json), &d,
                                             wifi_mgr_is_connected());
        if (n > 0) {
            /*
             * ★ 格式串必须是字面量 "%s"，JSON 从**可变参数**传进去。
             *
             *   绝对不要写成 ESP_LOGI(TAG, json)：那是把报文当成了格式串。
             *   报文里一旦出现 '%'（比如将来 device_id 里带了百分号），
             *   printf 就会去可变参数列表里找并不存在的参数，
             *   轻则打印乱码，重则读越界、直接崩栈。
             *
             *   一句话：**报文是数据，不是格式**。
             */
            ESP_LOGI(TAG, "收到 seq=%u: %s", (unsigned)d.seq, json);
        }
        /* n <= 0 时什么都不打：格式化函数内部已经说明原因了 */

        if (++cycles % STACK_REPORT_EVERY_CYCLES == 0) {
            app_report_stack(TAG);
        }
    }
}
