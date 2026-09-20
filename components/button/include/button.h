/*
 * button.h —— 按键组件的公开接口
 *
 * 这个组件演示 ESP-IDF 里"中断 + 任务间通信"的标准做法。
 *
 * 与 HAL 的对照（这是最关键的观念转变）：
 *
 *   HAL 的做法：配好 EXTI 中断，在中断回调（HAL_GPIO_EXTI_Callback）里
 *              直接干活——翻转 LED、发数据、改状态机。
 *
 *   ESP-IDF 的做法：中断服务程序（ISR）只做一件事——把事件塞进队列，
 *                  然后立刻返回。真正的处理交给一个普通任务，
 *                  任务从队列里取出事件再慢慢处理。
 *
 * 为什么要这样？三个原因：
 *   1. ISR 运行在中断上下文里，此时**调度器不能工作**，
 *      任何会阻塞的调用（malloc、printf、带超时的信号量）都会直接崩溃。
 *   2. ISR 会打断其他一切代码。在里面多待 1 微秒，系统的中断延迟就多 1 微秒。
 *   3. 按键有机械抖动，需要延时确认——而延时在 ISR 里是不可能做的。
 *
 * 所以本组件的分工是：
 *   ISR    —— 读电平、做最轻量的时间去抖、把事件丢进队列（几十条指令）
 *   任务   —— 从队列取出事件，做实际业务（用户自己写）
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 按键事件类型 */
typedef enum {
    BUTTON_EVENT_PRESSED,   /*!< 按下（电平变为有效电平） */
    BUTTON_EVENT_RELEASED,  /*!< 松开（电平回到无效电平） */
} button_event_t;

/** 按键配置 */
typedef struct {
    int gpio_num;           /*!< 按键连接的 GPIO 编号 */
    int active_level;       /*!< 按下时的电平：0 = 低电平有效，1 = 高电平有效 */
    uint32_t debounce_ms;   /*!< 去抖时间（毫秒），一般 20~50。
                                 下限受 tick 频率限制：100Hz 下必须 >= 10，
                                 否则换算后不足 1 个 tick，去抖会失效。
                                 见 button_init() 的说明。 */
} button_config_t;

/**
 * @brief 初始化按键并安装中断
 *
 * 内部会完成：配置 GPIO 为输入（按 active_level 自动启用上拉或下拉）、
 * 安装 GPIO 中断服务、注册本引脚的 ISR、创建事件队列。
 *
 * @param config 配置结构体，函数内会拷贝一份，调用方不必保持生命周期
 * @return
 *      - ESP_OK                  成功
 *      - ESP_ERR_INVALID_ARG     参数非法
 *      - ESP_ERR_INVALID_STATE   已经初始化过了
 *      - ESP_ERR_NO_MEM          内存不足
 */
esp_err_t button_init(const button_config_t *config);

/**
 * @brief 获取按键事件队列
 *
 * 调用方用 xQueueReceive() 从这个队列取事件，阻塞时间通常传 portMAX_DELAY。
 * 队列里的元素类型是 button_event_t。
 *
 * @return 队列句柄；未初始化时返回 NULL
 */
QueueHandle_t button_get_event_queue(void);

#ifdef __cplusplus
}
#endif
