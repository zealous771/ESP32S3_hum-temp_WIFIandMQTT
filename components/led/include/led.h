/*
 * led.h —— led 组件的公开接口
 *
 * 这是组件的"对外契约"。放在 include/ 目录下的头文件，才是其他组件能 #include 到的；
 * 如果某个头文件只想给自己用，放在 src/ 下，并在 CMakeLists 里用 PRIV_INCLUDE_DIRS 声明。
 *
 * 对照 HAL 的习惯：HAL 里你会把外设封装成 xxx.h + xxx.c 放进工程，
 * ESP-IDF 的组件机制是一样的思路，但多了一层"依赖声明"——
 * 谁用了谁，必须在 CMakeLists 里写清楚，构建系统据此决定编译顺序和头文件可见性。
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 LED 所在的 GPIO
 *
 * 会先把引脚复位到默认状态，再配置为推挽输出。
 *
 * @param gpio_num LED 连接的 GPIO 编号
 * @return
 *      - ESP_OK               成功
 *      - ESP_ERR_INVALID_ARG  编号非法
 */
esp_err_t led_init(int gpio_num);

/**
 * @brief 点亮 LED
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 表示尚未调用 led_init()
 */
esp_err_t led_on(void);

/**
 * @brief 熄灭 LED
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 表示尚未调用 led_init()
 */
esp_err_t led_off(void);

/**
 * @brief 翻转 LED 状态
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 表示尚未调用 led_init()
 */
esp_err_t led_toggle(void);

/**
 * @brief 查询 LED 当前是否点亮
 * @return true 点亮，false 熄灭或尚未初始化
 */
bool led_is_on(void);

#ifdef __cplusplus
}
#endif
