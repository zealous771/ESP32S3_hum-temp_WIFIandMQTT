/*
 * sensor_task.c —— 温湿度采样任务（生产者）
 *
 * 职责被刻意收窄成一件事：**按固定周期把一次采样结果放进队列**。
 *
 * 注意是**两条**队列：一条给串口打印（console_task），一条给 MQTT 上报
 * （mqtt_task）。为什么不是一条队列加两个消费者，见 app_internal.h
 * 里 "为什么是两条而不是一条" 的说明——简单说：xQueueReceive 是出队语义，
 * 两个消费者读同一条队列会变成瓜分数据，不是广播。
 *
 * 它不再格式化报文、不再打印、不查 WiFi 状态。那些都属于"输出"，
 * 现在归队列另一头的消费者（console_task，将来是 mqtt_task）。
 *
 * 为什么这个拆分不是形式主义？看失败模式就清楚了：
 *
 *   拆分前（单任务）：采样 → 格式化 → ESP_LOGI 全在一条时间线上。
 *     串口在 115200bps 下打一行几十字节要几毫秒；如果将来把
 *     "发布 MQTT"塞进同一处，一次 TCP 重传就是几百毫秒——
 *     **采样周期会被输出拖垮**。
 *
 *   拆分后：采样任务只做两件极短的事——读传感器、把几十字节的
 *     结构体拷进队列。输出侧的阻塞全部发生在消费者任务里，
 *     与采样节奏彻底解耦。这正是文档 §6 那句
 *     "采样任务不直接负责网络发送"要解决的问题。
 *
 * ★ 队列满时丢数据，绝不阻塞
 *
 *   xQueueSend() 的超时参数传的是 **0**，不是 pdMS_TO_TICKS(100)。
 *   这不是图省事，而是本任务最核心的设计决定：
 *
 *     传 0              → 队列满立刻返回失败，这条丢掉，下一轮照常采样
 *     传有限超时        → 消费者卡住时，采样任务被拖在这里等，周期被拉长
 *     传 portMAX_DELAY  → 采样任务彻底停摆
 *
 *   也就是说，"丢一条数据"和"采样节奏失控"之间必须选前者。
 *   对监测类系统而言，**节奏稳定比不丢数据重要得多**：周期乱掉的
 *   时间序列事后基本是废数据，而丢一条只是曲线上的一个空洞，
 *   并且 seq 跳号会如实记录下这件事。
 *   （真要"一条不丢"，正确做法是把队列换成 NVS/Flash 落盘，
 *     而不是让生产者在这里等——见文档 §37 的后续升级。）
 *
 *   两条队列各自独立丢弃，计数也分开记：串口侧丢说明打印被拖住了，
 *   MQTT 侧丢说明网络断了（mqtt_task 在未连接时故意不消费）。
 *   合成一个计数器的话，这两种完全不同的故障在日志里长得一模一样。
 *
 * 三个沿用下来的判断（原 telemetry_task 里的结论，继续有效）：
 *
 *   A. 用 vTaskDelay，不用"拿队列超时当延时"那个手法。
 *      那个手法的前提是**有事件源**（blink 要能被按键改周期）。
 *      本任务没有事件源，套用那种写法等于凭空造一条永远没人写的
 *      队列，只为让两个文件长得像——那是形式主义。
 *
 *   B. 失败只记 ESP_LOGW，不记 ESP_LOGE。
 *      DHT11 是"辅助功能"：它挂了，LED/按键/RGB 照样跑。
 *      给辅助功能打 ERROR，会让日志里 ERROR 这个词贬值。
 *
 *   C. 读失败时不往队列里塞"上一次的值"。
 *      塞旧值会被消费者当成这一轮的新采样；塞 0 会被当成真的 0℃。
 *      两种都是**用假数据填满管道**，比丢一条坏得多。所以失败就
 *      什么都不发，管道里出现空洞，由 seq 跳号如实体现。
 *
 * 注意：任务的**名称 / 栈大小 / 优先级不在这里**——统一放在 app.c。
 * "一个任务占多少资源"和"它干什么活"分开放，调整时不用翻遍所有文件。
 */
#include "app_internal.h"
#include "data_model.h"

#include <string.h>

#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "dht11.h"

static const char *TAG = "sensor";

/*
 * 采样周期的兜底下限（毫秒），只在上下文里的值异常时生效。
 *
 * 为什么是 2000：DHT11 手册要求两次读取间隔大于 2 秒。关键不在于
 * "慢一点更省事"，而在于**读太快不会报错**——传感器会返回上一次的
 * 数据或垃圾值，而不是返回错误码。这种"看起来正常、实际是旧值"
 * 的问题最难排查。
 *
 * 正常周期由 app.c 通过上下文的 sample_interval_ms 提供
 * （Kconfig 的范围也卡在 2000 起），这里只是最后一道保险。
 */
#define SAMPLE_INTERVAL_MIN_MS 2000

/* 每多少轮上报一次栈水位。采样周期默认 2000ms，约 10 秒报一次。 */
#define STACK_REPORT_EVERY_CYCLES 5

/*
 * 连续失败多少次之后，额外给一条"去看看硬件"的提示。
 * 取 5：按 2 秒一轮算，约 10 秒还没读通，基本可以排除偶发干扰了。
 */
#define FAIL_STREAK_HINT 5

/*
 * 队列满导致的丢弃，每累计这么多条提示一次。
 *
 * 为什么不是每条都提示：队列满意味着消费者跟不上，而"跟不上"是
 * 持续状态——每条都打会把串口刷爆，反而盖住真正的原因。
 * 为什么不完全静默：丢弃次数是判断"消费者是不是卡死了"的唯一线索，
 * 一点都不报，管道堵住时串口会安静得像一切正常。
 */
#define DROP_HINT_EVERY 10

/**
 * @brief 把一条记录投给某条队列，满了就丢并计数
 *
 * 为什么两条队列共用一个函数：投递策略完全一样（0 超时、满就丢、计数、
 * 限速告警），差别只有"投给谁"。写成函数是为了让下面主循环里那两行
 * 一眼能看出"是同样的处理"，而不是两段看起来像复制粘贴的代码。
 *
 * @param q       目标队列
 * @param d       要投递的记录
 * @param dropped 该队列的累计丢弃计数（调用方持有，本函数只递增）
 * @param who     队列名，只用于日志。必须与 ctx 里的字段名对得上，
 *                否则日志会把人引到错误的队列上
 */
static void send_to_queue(QueueHandle_t q, const sensor_data_t *d,
                          uint32_t *dropped, const char *who)
{
    /*
     * 超时参数 0 = 队列满就丢，绝不阻塞。理由见文件头。
     *
     * 注意这里用 != pdTRUE 判断，而不是 ESP_ERROR_CHECK 之类——
     * 丢数据是**预期内的正常分支**（消费者卡住时必然发生），
     * 不是编程错误，更不该触发 abort。
     */
    if (xQueueSend(q, d, 0) != pdTRUE) {
        if (++(*dropped) % DROP_HINT_EVERY == 0) {
            ESP_LOGW(TAG, "%s 队列已满，累计丢弃 %u 条（消费者跟不上？）",
                     who, (unsigned)*dropped);
        }
    }
}

void app_sensor_task(void *arg)
{
    /* pvParameters 传进来的是 &s_ctx，在这里取回自己的上下文 */
    const app_ctx_t *ctx = (const app_ctx_t *)arg;

    /*
     * 采样周期。0 是灾难性的，必须先挡掉再进循环：
     *   pdMS_TO_TICKS(0) == 0，vTaskDelay(0) 等于"立刻让出一次 CPU"
     *   马上又回来。于是本任务退化成不带任何延时的死循环：
     *   一边以 CPU 全速锤 DHT11 的单总线（每次都撞驱动的 min_interval
     *   守卫），一边以同样速度刷日志。
     *
     *   这里做防御性兜底并**明确告警**——静默改小一个配置项比报错更糟，
     *   用户会以为"我配的就是这个值"。
     */
    uint32_t interval_ms = ctx->sample_interval_ms;
    if (interval_ms == 0) {
        ESP_LOGW(TAG, "采样周期为 0 非法（会变成无延时的死循环），改用下限 %d ms",
                 SAMPLE_INTERVAL_MIN_MS);
        interval_ms = SAMPLE_INTERVAL_MIN_MS;
    }

    /*
     * 记录模板。
     *
     * 为什么在栈上建一个 sensor_data_t 反复用，而不是每轮新建？
     *   它就是个搬运用的中间对象：填好字段 → 拷进队列 → 下一轮覆盖。
     *   放栈上语义最准（它是本任务的私有状态），也避免了 static 带来的
     *   "哪天这里变成两个实例就串味"的隐患。函数是 while(1) 死循环
     *   不返回，局部变量的生命周期和任务一样长。
     *
     * device_id 只拷一次：它是常量，每轮重复拷贝纯属浪费。
     * 先 memset 再 strncpy 是因为 strncpy 在源串不短于目标缓冲时
     * **不会**补结尾 '\0'，memset 保证了这一点。
     */
    sensor_data_t d;
    memset(&d, 0, sizeof(d));
    strncpy(d.device_id, ctx->device_id, sizeof(d.device_id) - 1);

    uint32_t seq         = 0;
    uint32_t fail_streak = 0;
    uint32_t cycles      = 0;

    /*
     * 两条队列各自的丢弃计数。**分开记是有意的**：
     * 它们指向完全不同的故障——
     *   console 侧丢 = 串口打印被拖住了（不该发生，打印很快）
     *   mqtt 侧丢    = 网络断了、mqtt_task 故意不消费（预期内的正常现象）
     * 合成一个计数器的话，这两种情况在日志里长得一模一样。
     */
    uint32_t dropped_console = 0;
    uint32_t dropped_mqtt    = 0;

    ESP_LOGI(TAG, "采样任务启动，设备 %s，采样周期 %u ms", d.device_id,
             (unsigned)interval_ms);

    while (1) {
        dht11_reading_t r;
        const esp_err_t ret = dht11_read(&r);

        if (ret == ESP_OK) {
            /* 读通了就把欠账清零 */
            fail_streak = 0;

            /*
             * 时间戳取单调时钟。esp_timer_get_time() 返回微秒，
             * 除以 1000 得毫秒。它的分辨率实际是 1µs（硬件 62.5ns
             * 被整除截断），对秒级的采样周期绰绰有余。
             * 换成 Unix 时间要等 NTP，见 data_model.h 的说明。
             */
            d.timestamp_ms = esp_timer_get_time() / 1000;
            d.temperature  = r.temperature;
            d.humidity     = r.humidity;
            d.seq          = ++seq;

            /*
             * 分别投给两条队列。
             *
             * 顺序上**先串口后 MQTT**，但不是因为顺序重要（两条队列互不
             * 影响），而是因为读代码时先看到"这条数据一定会被打印出来"，
             * 心里更踏实——调试时串口那一路是最可靠的观测手段。
             *
             * 注意这不是"广播"：xQueueSend 是各投一份，两条队列各自
             * 持有一份独立的拷贝。这也是为什么不能用一条队列加两个
             * 消费者（那会变成瓜分），见 app_internal.h 里的说明。
             */
            send_to_queue(ctx->console_queue, &d, &dropped_console, "console");
            send_to_queue(ctx->mqtt_queue,    &d, &dropped_mqtt,    "mqtt");
        } else if (ret == ESP_ERR_INVALID_STATE) {
            /*
             * 这个错误码有两个来源：
             *   1. 驱动尚未初始化——那是 app.c 的编排问题，第一次运行
             *      就会暴露，不该由本任务每轮重复报告；
             *   2. 距上次读取还没到 min_interval_ms——**完全正常**，
             *      是驱动在替我们守时序，不是故障。
             * 两种都不在这里打日志：尤其第 2 种，每轮都可能发生，
             * 一打日志就会刷屏，真正的故障反而被淹掉。
             */
        } else {
            /* 其余都是真失败：超时（器件没应答）、校验错、时序非法 */
            fail_streak++;

            if (fail_streak == FAIL_STREAK_HINT) {
                /*
                 * 连续失败到阈值时，额外给一条指向**硬件**的提示。
                 *
                 * 为什么用 == 而不是 >= 或 %：表示"只在刚好跨过第 5 次时
                 * 提示一次"，之后不再重复。失败持续下去时下面那条 WARN
                 * 已经在如实报数了，每轮再刷一遍"检查接线"只会淹没日志。
                 *
                 * 为什么先怀疑上拉电阻：DHT11 数据线是**开漏**的，靠外部
                 * 上拉把电平拉高。三针模块自带这颗电阻，裸传感器（四针）
                 * 必须自己接 4.7kΩ~5.1kΩ。漏接的典型症状正是"偶尔能读通、
                 * 大部分时候超时"。其次才是接线松动、线太长（>100cm 时
                 * 上升沿变缓）。
                 */
                ESP_LOGW(TAG,
                         "连续 %u 次读不到 DHT11：请检查 DATA 线是否接好，"
                         "以及是否漏了 4.7kΩ~5.1kΩ 上拉电阻"
                         "（开漏总线没有上拉就出不了上升沿）",
                         (unsigned)fail_streak);
            } else {
                ESP_LOGW(TAG, "读取失败（%s），连续 %u 次；本轮不入队",
                         esp_err_to_name(ret), (unsigned)fail_streak);
            }
        }

        /*
         * 栈水位上报。
         *
         * 本任务的栈用量比拆分前小了不少——格式化 JSON 那段最吃栈的
         * 代码已经搬到 console_task 了。这里继续上报是为了**用数据确认
         * 这一点**：如果水位仍然接近 0，说明拆分没有达到预期效果，
         * 或者还有别的栈开销没被识别出来。调栈大小靠这个数，不靠猜。
         */
        if (++cycles % STACK_REPORT_EVERY_CYCLES == 0) {
            app_report_stack(TAG);
        }

        /*
         * 为什么这里是 vTaskDelay，而 blink_task 用的是
         * "xQueueReceive 的超时参数当延时"？
         *
         *   那个手法的关键前提是**存在一个事件源**：blink 的周期会被按键
         *   改，所以它必须能在任意时刻被叫醒，"定时"和"响应事件"才能
         *   合并成一次带超时的阻塞调用。
         *
         *   本任务没有事件源——没有任何东西会在中途叫它，周期也不是
         *   运行期可变的。硬套队列写法就得凭空造一条永远没人写的队列。
         *   判断顺序应该是：先问有没有事件源，再选等待方式；
         *   而不是先看别的任务怎么写。
         *
         * 顺带说时序上的"双保险"：
         *   DHT11 两次读取之间需要多秒间隔。这里有两道防线：
         *     1) 本任务的 interval_ms（正常 2000ms）；
         *     2) 驱动内部的 min_interval_ms（来自 dht11_config_t）。
         *   两道都留着不是冗余而是分工：任务侧的延时保证不会无谓地
         *   空转敲门；驱动侧的守卫保证**别的调用方**乱调时也不会破坏
         *   器件时序。守卫拦截时返回 ESP_ERR_INVALID_STATE，上面的分支
         *   已按"正常情况"静默处理。
         */
        vTaskDelay(pdMS_TO_TICKS(interval_ms));
    }
}
