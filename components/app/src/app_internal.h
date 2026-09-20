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
     * telemetry_task 用它当循环延时。注意 dht11 驱动内部还有一道
     * min_interval_ms 校验兜底——两边都设成同一个值是有意的：
     * 任务这边负责"节奏"，驱动那边负责"防呆"。
     */
    uint32_t sample_interval_ms;
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
void app_telemetry_task(void *arg);

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
