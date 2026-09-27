/*
 * app_internal.h —— app 组件内部共享的声明
 *
 * 这个文件放在 src/ 而不是 include/，含义是"不对外暴露"：
 *
 *   include/   → 公开接口，别的组件能 #include 到
 *                （靠 CMakeLists 的 INCLUDE_DIRS 声明）
 *   src/       → 私有头文件，只有本组件自己的 .c 能 #include 到
 *                （靠 CMakeLists 的 PRIV_INCLUDE_DIRS 声明）
 *
 * 外部使用者只应该看到 app.h。任务函数的原型、任务间共享的上下文，
 * 都属于"实现细节"，放这里。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "data_model.h"

/**
 * @brief 任务编号，用于索引 app_ctx_t::task_handles
 *
 * 加新任务的流程里多了一步：在这里加一个枚举值。
 * 忘了加的话，下面 task_handles[APP_TASK_COUNT] 会因越界而编译失败
 * ——这是好事，比"静默少监控一个任务"强。
 */
typedef enum {
    APP_TASK_BLINK = 0,
    APP_TASK_BUTTON,
    APP_TASK_RGB,
    APP_TASK_SENSOR,
    APP_TASK_CONSOLE,
    APP_TASK_MQTT,
    APP_TASK_MONITOR,
    APP_TASK_COUNT          /*!< 哨兵，必须留在最后 */
} app_task_id_t;

/**
 * @brief 应用的运行期上下文
 *
 * 由 app_start() 填好后，通过 xTaskCreate() 的 pvParameters 参数传给每个任务。
 * 这是 FreeRTOS 里传递任务参数的标准做法——相比用全局变量，
 * 它把"这个任务需要什么"写进了函数的参数，一眼能看出依赖关系。
 *
 * 生命周期要求：必须比所有任务活得久。所以定义成 app.c 里的 static 变量，
 * 不能是 app_start() 的局部变量（函数返回后栈就失效了，而任务还在跑）。
 */
typedef struct {
    const uint32_t *periods_ms;   /*!< 周期表，只读 */
    size_t          period_count; /*!< 周期表长度 */

    /*
     * 启动时使用的周期下标。
     *
     * 来自 NVS——上次按键切到哪一档，这次开机就从哪一档继续。
     * 由 app_start() 从 settings 组件读出周期值，再在周期表里
     * 反查出下标（查不到就用 0）。
     *
     * 为什么要存下标而不是周期值？
     *   blink_task 和 button_task 都需要知道"当前在第几档"——
     *   blink 用它取周期，button 用它算下一档。
     *   在上下文里存下标，两个任务都能直接用，不用各自换算。
     */
    uint32_t        initial_period_index;

    /*
     * 周期切换请求队列：button_task 写入新周期，blink_task 读出。
     *
     * 注意这是"任务 → 任务"的通信，和 button 组件内部那条
     * "ISR → 任务"的事件队列是两回事，不要混淆：
     *   button 组件的 s_event_queue —— ISR → button_task（传按键事件）
     *   ctx->period_queue           —— button_task → blink_task（传新周期）
     */
    QueueHandle_t period_queue;

    /*
     * 温湿度采样周期，毫秒。
     *
     * app_sensor_task 用它当循环延时。注意 dht11 驱动内部还有一道
     * min_interval_ms 校验兜底——两边都设成同一个值是有意的：
     * 任务这边负责"节奏"，驱动那边负责"防呆"。
     */
    uint32_t sample_interval_ms;

    /*
     * 设备标识，例如 "esp32s3_001"。
     *
     * 由 app_start() 从 app_config_t 拷进来，指向 main.c 里的字符串字面量，
     * 生命周期是全程的，所以存指针就够。采样任务会把它填进每条
     * sensor_data_t，最终出现在上报报文里。
     */
    const char *device_id;

    /*
     * 采样管道的两条队列。**它们装的是同样的内容，但互不干扰**。
     *
     * ★ 为什么是两条而不是一条？
     *
     *   FreeRTOS 的队列是**出队**语义：xQueueReceive() 会把元素从队列里
     *   摘走。所以"两个任务读同一条队列"不是广播，而是**瓜分**——
     *   每条记录只会落到其中一个任务手里，两个消费者各自都会看到
     *   大约一半的数据缺失。这是个很容易踩的坑，因为它在低负载下
     *   表现得像"偶尔丢一两条"，很难联想到队列语义。
     *
     *   要实现"一份数据、多个消费者都拿到"，只有两条路：
     *     ① 每个消费者一条独立队列，生产者分别投递（本工程的做法）
     *     ② 生产者投一条队列，由一个中间任务读出后再分发给下游
     *   ①更简单，也不引入额外任务，代价只是多一份队列内存。
     *
     * 为什么两条队列的分工正好是"串口"和"MQTT"：
     *
     *   console_queue —— 串口打印用。**永不阻塞消费**，所以它永远是空的，
     *                    传感器投递永远成功。它的存在让"采样是否还在跑"
     *                    这件事在断网时依然可见。
     *
     *   mqtt_queue    —— 上报用。mqtt_task 在**未连接时故意不消费**，
     *                    于是它会填满，sensor_task 对它投递失败并计数。
     *                    这样"网络断了导致丢数据"只影响上报这一路，
     *                    不会让串口那一侧也安静下来——两种故障因此
     *                    可以被区分开。
     *
     * 两条队列的元素都是**按值拷贝**的 sensor_data_t（不是指针），
     * 所以元素在队列里是自包含的，取出时不需要关心生产者的栈。
     *
     * 本工程一共四条队列，语义各不相同，别混：
     *   button 组件的 s_event_queue —— ISR → button_task（按键事件）
     *   ctx->period_queue           —— button_task → blink_task（新周期）
     *   ctx->console_queue          —— sensor_task → console_task（打印）
     *   ctx->mqtt_queue             —— sensor_task → mqtt_task（上报）
     */
    QueueHandle_t console_queue;
    QueueHandle_t mqtt_queue;

    /*
     * 系统监控任务的周期，毫秒。
     *
     * 来自 app_config_t（Kconfig 的 CONFIG_MONITOR_INTERVAL_MS）。
     * 只有 monitor_task 用得到，但它和其它任务参数一样统一走上下文，
     * 免得监控任务去 include sdkconfig.h——那会让 app 组件重新依赖
     * "本工程的 Kconfig"，破坏"组件可整体复制到别的工程"这个性质。
     */
    uint32_t monitor_interval_ms;

    /*
     * 各任务的句柄，按 app_task_id_t 索引。
     *
     * 为什么要存：monitor_task 需要打印**每个任务**的栈水位。
     * uxTaskGetStackHighWaterMark() 可以传别人的句柄，但前提是拿到句柄——
     * 而 xTaskCreate() 的输出参数目前都传的是 NULL。
     *
     * 另一条路是用 uxTaskGetSystemState() 枚举全部任务，但那需要打开
     * CONFIG_FREERTOS_USE_TRACE_FACILITY，还要准备一块按任务数动态变化的
     * 缓冲。为了打印几个数字付这个代价不值得。
     *
     * 数组大小由 APP_TASK_COUNT 保证，加任务时枚举和 xTaskCreate 都要改，
     * 漏改会编译报错（数组越界是编译期错误）而不是静默少打一行。
     */
    TaskHandle_t task_handles[APP_TASK_COUNT];
} app_ctx_t;

/*
 * 各任务的入口函数。
 *
 * 加新任务的流程：在这里加一行原型 → 新建 xxx_task.c 实现 → 在 app.c 里加一次
 * xTaskCreate()。入口文件 main.c 全程不用动。
 */
void app_blink_task(void *arg);
void app_button_task(void *arg);
void app_rgb_task(void *arg);

/*
 * 采样管道被拆成"一个生产者 + 两个消费者"：
 *
 *   app_sensor_task   读传感器 → 同时投递到两条队列（只管采集，不碰输出）
 *   app_console_task  console_queue → 串口 JSON（不依赖网络，常驻诊断通道）
 *   app_mqtt_task     mqtt_queue → publish 到 broker（断网时不消费，让队列积压）
 *
 * 为什么不是一个任务干完？见 sensor_task.c 文件头——核心是
 * "采样节奏不能被任何输出侧的阻塞拖垮"。
 * 为什么是两个消费者而不是一个？见上面两条队列的说明。
 */
void app_sensor_task(void *arg);
void app_console_task(void *arg);
void app_mqtt_task(void *arg);

/**
 * @brief 系统监控任务（文档 §38）
 *
 * 纯观测，不参与任何业务：周期性地把堆、各任务栈水位、WiFi/MQTT 状态、
 * 传感器错误计数、复位原因打成一行。它的优先级**低于**所有业务任务，
 * 因为"看"永远不该和"干"抢资源。
 */
void app_monitor_task(void *arg);

/**
 * @brief 采样侧的运行统计，供监控任务读取
 *
 * 这些数字是排查"数据为什么不对"的第一手线索：
 *   读失败多   → 硬件/接线问题（看 sensor_task 里的上拉电阻提示）
 *   串口丢弃多 → 打印被拖住（不该发生）
 *   MQTT 丢弃多 → 网络断了（预期现象，但要能看见）
 *   两条队列丢弃都多 → 采样本身太快，或者系统整体过载
 */
typedef struct {
    uint32_t reads_ok;         /*!< 累计读取成功次数 */
    uint32_t reads_failed;     /*!< 累计读取失败次数（不含"未到最小间隔"） */
    uint32_t dropped_console;  /*!< 因 console_queue 满而丢弃的条数 */
    uint32_t dropped_mqtt;     /*!< 因 mqtt_queue 满而丢弃的条数 */
} app_sensor_stats_t;

/**
 * @brief 读取采样侧的运行统计
 *
 * 可以从任意任务调用。内部只是把几个 uint32 拷出来，不加锁——
 * 理由见 sensor_task.c 里的说明（对齐的 32 位读在 Xtensa 上不会撕裂，
 * 而监控数据允许存在"跨字段不一致"的窗口，为它加锁不划算）。
 *
 * @param out 输出。为 NULL 时什么也不做
 */
void app_sensor_get_stats(app_sensor_stats_t *out);

/**
 * @brief 记录**当前任务**的栈使用水位
 *
 * 由各任务在循环里周期性调用。必须在任务自己的上下文里调——
 * 内部用的是 uxTaskGetStackHighWaterMark(NULL)，传 NULL 表示"我自己"。
 *
 * 单位陷阱（重要）：
 *   ESP-IDF 里这个函数返回的是**字节**，不是标准 FreeRTOS 文档说的"字"。
 *   因为 Xtensa 端口的 portSTACK_TYPE 被定义成了 uint8_t：
 *     #define portSTACK_TYPE  uint8_t      // portmacro.h:78
 *     typedef portSTACK_TYPE StackType_t;
 *   而 xTaskCreate 的栈参数同样是字节。两边单位一致，所以**不要**
 *   再乘 sizeof(StackType_t) 去换算——那只会乘出错误的结果。
 *
 * @param tag 日志标签，一般直接传任务文件里的 TAG
 */
void app_report_stack(const char *tag);
