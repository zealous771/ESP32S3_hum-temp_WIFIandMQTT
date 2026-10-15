/*
 * console_task.c —— 串口消费者
 *
 * 它从 **console_queue** 取出记录，格式化成一行 JSON 打到串口。
 *
 * ★ 它和 mqtt_task 是**各自独立的消费者**，读的是两条不同的队列
 *   （sensor_task 分别投递，各持一份拷贝）。这不是重复劳动——
 *   原因见下。
 *
 * ── 为什么需要一个不依赖网络的观测点 ────────────────────────────
 *
 *   接 MQTT 之后，mqtt_task 会把数据发出去。但它的输出要经过 broker
 *   才能看到——而 broker 没起、Windows 防火墙拦住 1883、Mosquitto 只
 *   监听 127.0.0.1、SSID 填错，任何一个环节出问题，你都会"什么都看不到"，
 *   并且无法判断到底是采样停了、队列断了，还是仅仅网络不通。
 *
 *   console_task 不看网络。它只要能打印，就证明
 *   「传感器 → 采样任务 → 队列」这一段是好的。
 *   排错时它把"本地链路"和"网络链路"彻底分开——
 *   所以接上 MQTT 之后**不应该删掉它**，它是常驻的诊断通道。
 *
 *   更关键的是**断网时两条队列的行为不同**（见 app_internal.h）：
 *   mqtt_task 在未连接时故意不消费，它的队列会填满、sensor 对它丢弃；
 *   而 console_queue 始终被及时排空。于是"网络断了导致丢数据"这件事
 *   只影响上报那一路，串口这边照常一行一行地打——两种故障因此
 *   可以被区分开。如果只有一个消费者，断网时串口会一起安静下来，
 *   你就再也分不清是"网络问题"还是"采样死了"。
 *
 * ── 阻塞策略 ───────────────────────────────────────────────────
 *
 *   生产者 xQueueSend(..., 0)              队列满就丢，保住采样节奏
 *   消费者 xQueueReceive(..., portMAX_DELAY) 队列空就无限等，不空转
 *
 *   两边各取所需：一个要"绝不等待"，一个要"绝不空转"。
 *   消费者若改用"0 超时轮询"，会在队列空时以 CPU 全速空跑——
 *   它和其它任务同为优先级 5，空转虽然会让出 CPU，但白白消耗调度。
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
        if (xQueueReceive(ctx->console_queue, &d, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        /*
         * 组装报文。环境信息（wifi / rssi / 时间基准）由 app_collect_meta()
         * 现取——它和 mqtt_task 调的是同一个函数，所以两边打出来的报文
         * **逐字节相同**。对不上就说明有问题，这本身就是一条有用的性质。
         *
         * 缓冲给 192 字节。当前报文最长约 163 字节
         * （device_id 31 + ts 19 位 + seq 10 位 + rssi 4 位 + 各字段与语法字符），
         * 余量留足，免得将来再加字段就要回头改这里。
         */
        telemetry_meta_t meta;
        app_collect_meta(&meta, d.timestamp_ms);

        char json[192];
        const int n = app_format_reading_json(json, sizeof(json), &d, &meta);
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
