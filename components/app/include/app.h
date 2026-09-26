/*
 * app.h —— 应用层组件的公开接口
 *
 * 为什么要有这一层？
 *
 *   main.c 是"入口"，不是"应用逻辑的存放处"。ESP-IDF 里 main 是一个特殊组件：
 *   它总是被编译、总是第一个运行，但它的职责应该只有两件事——
 *     1. 声明板级配置（引脚号、周期表这类"这台机器特有的信息"）
 *     2. 把控制权交给应用层
 *
 *   把所有任务都堆在 main.c 里会带来两个后果：
 *     - 每加一个任务，入口文件就长一截，很快变得难以通读
 *     - 应用逻辑和入口耦合在一起，想搬到别的工程要整体复制
 *
 *   所以拆成：
 *     main.c          板级配置 + 一行启动调用（换板子只改这里）
 *     components/app  应用逻辑：初始化外设、建队列、创建任务
 *
 * 关键设计：app 组件本身不含任何"这台板子特有"的信息。
 * 引脚接在哪、闪烁周期有几档，全部通过 app_config_t 从外部注入。
 * 这样 app 组件可以直接复制到别的工程用。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 应用配置
 *
 * 由调用方（main.c）填写。组件内部不假设任何默认值——
 * 引脚、周期都是板级信息，组件不该替调用方决定。
 */
typedef struct {
    int led_gpio;                 /*!< 普通 LED 所在 GPIO */
    int led_active_level;         /*!< LED 点亮时的电平：1 高电平点亮，0 低电平点亮 */

    int button_gpio;              /*!< 按键所在 GPIO */
    int button_active_level;      /*!< 按下时的电平：0 低电平有效，1 高电平有效 */
    uint32_t button_debounce_ms;  /*!< 按键去抖时间，毫秒 */

    int rgb_gpio;                 /*!< 可寻址 RGB（WS2812B）数据线 GPIO */
    uint32_t rgb_led_count;       /*!< 可寻址 RGB 的灯珠数量，板载那颗是 1 */

    int dht11_gpio;               /*!< DHT11 温湿度传感器的单总线数据引脚 */
    uint32_t sample_interval_ms;  /*!< 温湿度采样周期；DHT11 手册要求 > 2000 */

    /*
     * 设备标识，例如 "esp32s3_001"。
     *
     * 为什么不在这里生成、而要调用方给？
     *   因为它属于"这台机器是谁"——板级/部署级信息，和引脚号同类。
     *   组件若自己用 MAC 地址拼一个，就替调用方做了一个它不该做的决定
     *   （有人希望用位置命名、有人希望用 MAC 后六位、有人要跟资产编号对齐）。
     *
     * 为什么是 const char * 而不是定长数组：与下面 WiFi 凭据同理，
     * 组件只是把它拷进 sensor_data_t 的定长缓冲，不必在这里再存一份。
     * 字符串字面量的生命周期是全程的，指针足够。
     *
     * 长度上限 31 字节（sensor_data_t.device_id 是 char[32]），超长会被截断。
     */
    const char *device_id;        /*!< 设备标识，用于上报报文中的 device_id 字段 */

    /*
     * WiFi 凭据。
     *
     * 为什么是 const char * 而不是定长数组？
     *   数组会把"最长 32/64 字节"这个空间代价带进结构体，而组件只是
     *   看一眼就把它拷进 WiFi 驱动的定长配置里，没有必要在这里再存一份。
     *   字符串字面量的生命周期是全程的，所以指针足够。
     *
     * ssid 传空串表示"不联网"——这是受支持的用法，不是错误路径。
     * 设备会照常跑 LED / 按键 / RGB / 温湿度采集，只是不上报。
     */
    const char *wifi_ssid;        /*!< WiFi 名称；空串表示不联网 */
    const char *wifi_password;    /*!< WiFi 密码；开放网络传空串 */
    uint32_t wifi_connect_timeout_ms; /*!< 等待拿到 IP 的超时，毫秒 */

    const uint32_t *periods_ms;   /*!< 闪烁周期表，按键按下时循环切换 */
    size_t period_count;          /*!< 周期表长度，必须 >= 1 */
} app_config_t;

/**
 * @brief 启动应用：初始化外设、建立任务间通信、创建各任务
 *
 * 本函数返回后各任务已在运行，调用方（app_main）可以直接返回。
 *
 * @warning **失败时不做回滚**。本函数按"外设 → 队列 → 任务"的顺序推进，
 *          任何一步失败就立即返回错误，但**前面已经成功的步骤不会撤销**：
 *          比如外设初始化成功后队列创建失败，外设仍处于已初始化状态；
 *          又比如几个任务里第二个创建失败，第一个任务已经在跑了。
 *
 *          所以调用方**不应该**在失败后继续使用这个系统，也不该重试——
 *          重试会因为"已经初始化过了"而返回 ESP_ERR_INVALID_STATE。
 *          正确的处理是把它当作不可恢复的启动失败，让系统复位重来。
 *          本工程的 app_main 用 ESP_ERROR_CHECK() 正是这个意思：
 *          打印错误信息后 abort，由看门狗/复位重新走一遍。
 *
 *          （嵌入式里这样做是合理的：启动阶段失败通常意味着配置或硬件
 *           有问题，回滚到"什么都没发生"的状态既复杂又没什么意义。
 *           但如果你写的是可重入的库，那就必须做完整回滚。）
 *
 * @param config 配置。函数内只读取，调用方不必保持其生命周期
 * @return
 *      - ESP_OK               成功
 *      - ESP_ERR_INVALID_ARG  配置为 NULL、周期表为空、device_id 为 NULL
 *      - ESP_ERR_NO_MEM       队列或任务创建失败
 *      - 其他                 外设初始化失败时，透传各组件 init 的错误码
 *                             （led_init / button_init / rgb_led_init /
 *                              dht11_init）
 *
 * @note WiFi 有两条特殊约定：
 *
 *       1. **ssid 为空表示不联网，是受支持的用法**。此时本函数不会失败，
 *          只是 telemetry_task 的报文里 "wifi" 恒为 false。
 *          只有 WiFi 真正初始化失败（比如内存不足）才返回错误。
 *
 *       2. **等 IP 超时不算失败**。本函数会阻塞最多
 *          wifi_connect_timeout_ms 等一次 IP，好让第一行遥测报文就带上
 *          正确的 wifi 标志；超时只记一行告警就继续建任务。
 *          路由器后来自动连上时，wifi_mgr 的重连逻辑会把状态纠正过来。
 *          所以"启动耗时"的上界包含这个超时值。
 */
esp_err_t app_start(const app_config_t *config);

#ifdef __cplusplus
}
#endif
