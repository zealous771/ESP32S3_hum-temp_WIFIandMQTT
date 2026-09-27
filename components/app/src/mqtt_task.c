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
         * 用 portMAX_DELAY 阻塞等待。
         *
         * 为什么不需要超时：传感器每 2 秒就会产生一条数据，所以这个
         * 阻塞最多持续一个采样周期。而"断连时怎么办"已经在循环开头
         * 处理了——就算这里一直阻塞到采样停止，也没有数据要发，
         * 早发现断连没有任何意义。
         *
         * 反过来，如果用带超时的接收，就会在队列空时周期性醒来空转，
         * 白白消耗调度（与 console_task 里那段说明同一个道理）。
         */
        if (xQueueReceive(ctx->mqtt_queue, &d, portMAX_DELAY) != pdTRUE) {
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
            /* 格式化函数内部已经说明原因（缓冲不够），这里不重复报 */
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
             * 走到这里通常是"检查连接时还好、publish 时已经断了"的
             * 时间窗，或者 outbox 满。
             *
             * 注意**不重试**：下一条 2 秒后就到，为一条旧数据做重试
             * 逻辑会引入缓冲区、退避、上限一堆复杂度，收益却很小。
             * 真要保证不丢，靠的是"未连接时不消费队列"那套机制
             * （见文件头），而不是在这里重试。
             */
            if (++publish_failed % PUBLISH_FAIL_HINT_EVERY == 0) {
                ESP_LOGW(TAG, "上报失败累计 %u 条（最近一次 %s，seq=%u）",
                         (unsigned)publish_failed, esp_err_to_name(ret),
                         (unsigned)d.seq);
            }
        }

        if (++cycles % STACK_REPORT_EVERY_CYCLES == 0) {
            app_report_stack(TAG);
        }
    }
}
