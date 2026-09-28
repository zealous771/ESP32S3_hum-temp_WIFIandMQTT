/*
 * mqtt_task.c —— MQTT 上报任务（采样管道的第二个消费者）
 *
 * 职责：从 **mqtt_queue** 取出记录 → 组装报文 → publish 到 broker。
 *
 * 它和 console_task 是平级的两个消费者，各读各的队列
 * （sensor_task 分别投递，各持一份拷贝，理由见 app_internal.h）。
 *
 * ── ★ 未连接时故意不消费队列 ────────────────────────────────────
 *
 * 这是本任务最重要的一个行为决定。两种做法的对比：
 *
 *   做法一：照常取出来，publish 失败就丢掉
 *     队列不会积压，但数据当场没了。而且因为队列不积压，
 *     断网这件事在 sensor 侧完全不可见——seq 不跳号，
 *     事后看数据像是"那段时间没采样"，而不是"采了但没发出去"。
 *
 *   做法二（本任务采用）：未连接就不取，让队列自然填满
 *     队列深度 8 × 2 秒 ≈ 16 秒的短时缓存。网络几秒的抖动
 *     完全被吸收，重连后把积压按顺序补发出去，一条不丢。
 *     超过 16 秒的断网则由 sensor_task 对 mqtt 队列丢弃并计数，
 *     seq 跳号会如实体现——后端能看出"这段时间丢了多少条"。
 *
 * 为什么不用"继续取但不发"来保住队列不积压：那等于把数据从队列里
 * 搬到一个不存在的地方，纯粹是自欺欺人。
 *
 * ── ★ 补充：光"未连接时不取"还不够，取的动作本身也要延后 ────────
 *
 * "未连接就不取"依赖 mqtt_mgr_is_connected() 的准确。但它读的是
 * MQTT 的 CONNECTED/DISCONNECTED **事件**，而事件是滞后的：
 * broker 已经没了、本机 TCP 还没察觉时，它仍然返回 true。
 *
 * 这段时间里如果"取一条 → 发布失败 → 丢掉"，数据就无声无息地少了
 * （队列不积压，sensor 也不计数，事后只有 seq 跳号）。
 * 窗口最长可达 keepalive×1.5（本工程 90 秒，能丢几十条）。
 *
 * 所以本任务用 xQueuePeek 取代 xQueueReceive：**只有发布成功才真正
 * 出队**。这样"未连接时不取"和"取出来发不出去"这两种情况都不会
 * 丢数据，丢数据的唯一途径回到"队列被填满"——那是可见的。
 * 详见主循环里 peek 处的注释。
 *
 * ── 重连后补发积压数据，是对的吗？────────────────────────────────
 *
 * 对。因为队列里存的时间戳是**采样时刻**（而且离开设备时会被换算成
 * 正确的 Unix 毫秒），不是"发送时刻"。晚到 16 秒但时间戳正确，
 * 对时序数据来说完全可用；丢掉才是真的损失。
 *
 * 代价是重连瞬间会有一次最多 8 条的突发。8 条报文对 broker 不算什么。
 *
 * 如果断网很久（比如几小时），这个方案救不了——队列只有 16 秒的容量。
 * 那要靠文档 §37 提到的 NVS/SPIFFS 落盘，不在本轮范围内。
 *
 * ── 与 console_task 的分工 ──────────────────────────────────────
 *
 *   console_task：读 console_queue，永远在消费，永远在打印。
 *                 它的输出不依赖任何外部服务，是"本地链路是否活着"
 *                 的观测点。
 *   mqtt_task：  读 mqtt_queue，断网时停止消费。
 *                 于是"网络断了导致丢数据"只影响这一路，
 *                 串口那边照常一行一行地打——两种故障因此可区分。
 *
 * 注意：任务的**名称 / 栈大小 / 优先级不在这里**——统一放在 app.c。
 */
#include "app_internal.h"
#include "data_model.h"
#include "telemetry_json.h"

#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"

#include "mqtt_mgr.h"

static const char *TAG = "mqtt";

/*
 * 未连接时的轮询间隔。
 *
 * 为什么是轮询而不是"阻塞等连接事件"：mqtt_mgr 没有暴露"等连接"的
 * 无日志接口（wait_connected 每次超时都会打一条 WARN，每秒一次会刷屏）。
 * 1 秒轮询的代价可以忽略——任务绝大部分时间在 vTaskDelay 上睡着。
 *
 * 也不能用 portMAX_DELAY 在这里干等：那样就再也不会回来检查连接状态了。
 */
#define CONNECT_POLL_MS 1000

/* 每多少条上报后汇报一次栈水位 */
#define STACK_REPORT_EVERY_CYCLES 5

/*
 * 上报失败每累计这么多次提示一次。
 *
 * 限速的理由同 sensor_task 的丢弃计数：失败往往是持续状态
 * （broker 挂了、网络不通），每条都打会刷屏，反而盖住真正的原因；
 * 完全不报又会让"发不出去"这件事静默无声。
 */
#define PUBLISH_FAIL_HINT_EVERY 10

/*
 * 发布失败后重试前等多久（毫秒）。
 *
 * 注意这不是"重试次数上限"或"重试队列"——本任务的重试机制就是
 * "失败的那条留在队首不动"（见主循环里改用 xQueuePeek 的说明），
 * 天然带退避语义。这里的延时只做一件事：避免在持续失败时以全速
 * 空转（采样节奏是 2 秒一条，1 秒退避最多让重试频率翻倍，可忽略）。
 */
#define PUBLISH_RETRY_DELAY_MS 1000

/*
 * 报文缓冲。当前报文最长约 163 字节（device_id 31 + ts 19 位 +
 * seq 10 位 + rssi 4 位 + 各字段与语法字符），给到 192 留余量。
 */
#define JSON_MAX 192

void app_mqtt_task(void *arg)
{
    const app_ctx_t *ctx = (const app_ctx_t *)arg;

    /*
     * 启动时打一行当前状态。三种状态的含义见 mqtt_mgr_state_str()：
     *   disabled     —— 没配 broker，本任务会一直空转（每秒一次），
     *                   这是**预期状态**，不是故障
     *   disconnected —— 配了但还没连上，esp-mqtt 正在重试
     *   connected    —— 已连上
     */
    ESP_LOGI(TAG, "上报任务启动，MQTT 状态 %s", mqtt_mgr_state_str());

    sensor_data_t d;
    uint32_t      cycles         = 0;
    uint32_t      publish_failed = 0;

    while (1) {
        if (!mqtt_mgr_is_connected()) {
            /*
             * 未连接：**不取队列**，让它自然积压。
             * 详细的取舍见文件头"未连接时故意不消费队列"一节。
             *
             * 这里也不打日志：断线期间每秒一条会把串口刷满，
             * 而 esp-mqtt 自己已经在按 reconnect_timeout_ms 重试，
             * 并在 mqtt_mgr 里做了限速告警。这里保持安静。
             */
            vTaskDelay(pdMS_TO_TICKS(CONNECT_POLL_MS));
            continue;
        }

        /*
         * ★ 这里用 xQueuePeek 而**不是** xQueueReceive。
         *
         * 区别在于"取不取走"：xQueuePeek 只看队首、不移除；
         * xQueueReceive 会把元素摘掉。
         *
         * 为什么要这样——先取出再发布会造出一段**静默丢数据的窗口**：
         *
         *   broker 挂掉（或容器重启）之后、本机 TCP 察觉到之前，
         *   mqtt_mgr_is_connected() 仍然返回 true（MQTT 的
         *   CONNECTED/DISCONNECTED 事件还没来），本任务于是继续
         *   取数据、继续 publish 失败、继续把数据丢掉——而且队列
         *   不积压，sensor_task 那边也不会计数，**事后只有 seq 跳号**，
         *   看不出是"采了没发出去"还是"根本没采"。
         *
         * 这个窗口有多长，取决于断连怎么被发现：
         *   容器重启        → 收到 TCP RST      → 几秒
         *   网线拔掉/断电    → 靠 keepalive 超时 → 约 keepalive×1.5
         *                     （本工程 60 秒 → 最长 90 秒，能丢几十条）
         *
         * 实测就是这么暴露的：broker 容器重启那一下，seq 从 234
         * 直接跳到 237，中间两条无声无息地没了。
         *
         * 改成 peek 之后，**只有发布成功才真正出队**（见下面），
         * 失败就原样留在队首、下轮再试。于是"丢数据"的唯一途径
         * 回到"队列被填满"，而那是可见的——由 sensor_task 计数并告警，
         * seq 跳号也能对上账。
         *
         * 代价：每条数据多一次队列操作（几条指令），可忽略。
         *
         * 阻塞仍用 portMAX_DELAY，理由同前：传感器每 2 秒产生一条，
         * 这个阻塞最多持续一个采样周期；用带超时的接收只会在队列空时
         * 周期性空转，白白消耗调度（与 console_task 里那段说明同理）。
         */
        if (xQueuePeek(ctx->mqtt_queue, &d, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        /*
         * 组装报文。与 console_task 调的是同一个函数，所以两边
         * 发出去/打出来的内容**逐字节相同**。
         *
         * 环境信息（wifi / rssi / 时间基准）在这里现取，而不是由
         * sensor_task 在采样时填进 sensor_data_t —— RSSI 是"发出时刻"
         * 的网络事实，不是"采样时刻"的物理量，积压补发时更是错的。
         * 详见 data_model.h 里关于 rssi 的说明。
         */
        telemetry_meta_t meta;
        app_collect_meta(&meta, d.timestamp_ms);

        char json[JSON_MAX];
        const int n = app_format_reading_json(json, sizeof(json), &d, &meta);
        if (n <= 0) {
            /*
             * ★★ 格式化失败**必须显式出队**，否则这条永远卡在队首。
             *
             * 这是"peek + 成功后出队"这个模式必须配的纪律，很容易漏：
             *
             *   peek 不消费 → 下一轮拿到的还是同一条 → 又格式化失败 →
             *   又 continue …… 无限循环，而且这个循环里**没有任何延时**，
             *   会把 CPU 占满、把同优先级的其它任务挤到只剩时间片轮转。
             *
             * 表现极具迷惑性：MQTT 连接看起来是好的（esp-mqtt 的
             * PINGREQ 还在发），但**一条数据都发不出去**，串口上也没有
             * 任何错误——因为格式化函数把原因说过了，这里刻意没重复报。
             *
             * 所以：**peek 之后每一个"决定不处理这条"的分支，都要先出队。**
             * 目前本函数有三个 continue，纪律是这样的：
             *   - peek 返回失败  → 队列本来就空，无队可出
             *   - 格式化失败     → 出队丢弃（本条）
             *   - publish 失败   → **故意不出队**，留在队首重试（这才是 peek 的意义）
             */
            sensor_data_t discard;
            (void)xQueueReceive(ctx->mqtt_queue, &discard, 0);
            continue;
        }

        /*
         * QoS 1：至少一次送达。选它而不是 QoS 0 是因为温湿度数据
         * 丢一条就在曲线上留个洞；也不用 QoS 2——那要多两轮握手，
         * 对"2 秒一条、偶尔重复也无所谓"的场景不划算。
         *
         * retain = false：历史数据没有"保留"的意义。retain 是给
         * **状态类**消息用的（比如 status 的 online/offline），
         * 让新订阅者立刻拿到当前状态；telemetry 用 retain 只会让
         * 新订阅者收到一条过期的读数。
         */
        const esp_err_t ret = mqtt_mgr_publish(MQTT_MGR_SUFFIX_TELEMETRY,
                                               json, n, 1, false);
        if (ret != ESP_OK) {
            /*
             * 发布失败：**不把这条取出队列**，原样留在队首，下轮重试。
             *
             * 这正是上面改用 peek 的意义——失败的那条不会消失。
             * 如果断连持续，队首一直是它，队列很快被填满，然后由
             * sensor_task 丢弃并计数、seq 跳号如实体现。
             * "丢数据"这件事于是重新变成**可见的**。
             *
             * 退避 1 秒只是避免持续失败时全速空转，没有重试上限、
             * 不引入额外缓冲——重试状态就是"队首那条还在"。
             */
            if (++publish_failed % PUBLISH_FAIL_HINT_EVERY == 0) {
                ESP_LOGW(TAG, "上报失败累计 %u 条（最近一次 %s，seq=%u），"
                              "该条留在队首下轮重试",
                         (unsigned)publish_failed, esp_err_to_name(ret),
                         (unsigned)d.seq);
            }
            vTaskDelay(pdMS_TO_TICKS(PUBLISH_RETRY_DELAY_MS));
            continue;
        }

        /*
         * 发布成功，这时才真正把这条从队列里取走。
         *
         * 用超时 0 的 xQueueReceive：队首必然还是刚才 peek 到的那一条
         * ——本队列只有一个消费者，peek 到这里之间没有别的任务会动它
         * ——所以不会阻塞、也不会取错。
         *
         * 取出来的值丢弃不用：d 里已经有同样的内容，这次调用只是为了
         * "出队"这个副作用。
         */
        sensor_data_t consumed;
        (void)xQueueReceive(ctx->mqtt_queue, &consumed, 0);

        if (++cycles % STACK_REPORT_EVERY_CYCLES == 0) {
            app_report_stack(TAG);
        }
    }
}
