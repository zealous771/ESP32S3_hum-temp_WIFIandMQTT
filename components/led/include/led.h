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
#pragma once            //避免了头文件被重复包含
                        /*
                            功能等价：
                                #ifndef MY_HEADER_H
                                #define MY_HEADER_H
                                ...
                                #endif
                        */
#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus      //*实现CPP和C之间的链接，当使用CPP编译器时可以链接到C，而使用C时就会被掠过。花括号内写的是C包含的C头文件*//                       
extern "C" {
#endif

/**
 * @brief LED 配置
 *
 * 为什么要带 active_level？
 *
 *   同样是"点亮"，硬件上有两种完全相反的接法：
 *
 *     拉电流（高电平点亮）
 *       GPIO ──[限流电阻]──▶|── GND
 *       GPIO 输出高 → 电流从引脚流出 → 亮
 *
 *     灌电流（低电平点亮）
 *       3V3 ──[限流电阻]──▶|── GPIO
 *       GPIO 输出低 → 电流从引脚流入地 → 亮
 *
 *   两者只是把 LED 和电阻对调了位置，但代码里的电平要反过来写。
 *   如果把"有效电平"写死在驱动里，换一种接法就得改驱动源码——
 *   而这件事明明是**硬件决定**的，属于板级信息。
 *
 *   所以把它做成配置项：驱动负责"按配置输出正确电平"，
 *   接线是什么样由调用方告诉它。这和 button 组件里
 *   active_level 的处理是同一个道理。
 */
typedef struct {
    int gpio_num;       /*!< LED 连接的 GPIO 编号 */
    int active_level;   /*!< 点亮时的电平：1 = 高电平点亮（拉电流），0 = 低电平点亮（灌电流） */
} led_config_t;

/**
 * @brief 初始化 LED 所在的 GPIO
 *
 * 会先把引脚复位到默认状态，再配置为推挽输出，最后按 active_level 熄灭。
 *
 * @param config 配置。函数内只读取，调用方不必保持生命周期
 * @return
 *      - ESP_OK               成功
 *      - ESP_ERR_INVALID_ARG  config 为 NULL、引脚编号非法，或 active_level 不是 0/1
 *      - ESP_ERR_INVALID_STATE 已经初始化过了
 */
esp_err_t led_init(const led_config_t *config);

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
