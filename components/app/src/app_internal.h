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

#include "data_model.h"

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
