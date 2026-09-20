/*
 * settings.h —— 掉电不丢的小配置
 *
 * 这个组件解决一个具体问题：按键把闪烁周期切换成 500ms 之后，
 * 一断电就回到 1000ms 了。要让它记住，得把值写进**非易失存储**。
 *
 * ESP32 上的标准做法是 NVS（Non-Volatile Storage）。
 *
 * NVS 是什么？
 *
 *   一个跑在 Flash 上的键值数据库。它占用分区表里那个
 *   `nvs, data, nvs` 分区（本工程是 24KB @ 0x9000）。
 *   写入时以"命名空间 + 键名"定位，值可以是整数、字符串、二进制块等。
 *
 *   它解决的核心问题是：Flash 不能像 RAM 那样随便写。
 *     - 擦除粒度是 4KB 扇区，写之前必须先擦
 *     - 每个扇区的擦写寿命有限（典型 10 万次）
 *   直接操作 Flash 的话，每次改一个字节都要擦 4KB，
 *   而且同一个地址反复擦写会把它写坏。
 *
 *   NVS 在中间做了两层：
 *     1. **日志式写入**——新值追加写，旧值标记失效，而不是原地覆盖。
 *        这样避免了"擦一整块只为改几个字节"。
 *     2. **磨损均衡**——在多个扇区之间轮换，让擦写次数摊开。
 *
 *   代价是 NVS 不适合高频写入。按键这种"人手动触发"的频率完全没问题，
 *   但**不要**把它当循环里的计数器用。
 *
 * 为什么这里只存一个数字？
 *
 *   本组件是"够用就好"的示范，只暴露一个周期的存取。
 *   真实项目里一般会做成一张配置表（多个键），
 *   但结构完全一样——多几个键而已。
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 NVS
 *
 * 内部会调用 nvs_flash_init() 挂载分区。
 *
 * 注意有一种情况需要额外处理：如果 nvs 分区里的数据是**旧版本格式**
 * （比如 IDF 升级过、或分区表改过），nvs_flash_init() 会返回
 * ESP_ERR_NVS_NO_FREE_PAGES 或 ESP_ERR_NVS_NEW_VERSION_FOUND。
 * 这时标准做法是擦掉整个分区重新初始化——代价是丢配置，
 * 但不擦的话 NVS 根本用不了。本函数会自动做这件事。
 *
 * @return
 *      - ESP_OK  成功（含"擦除后重新初始化"成功）
 *      - 其他    透传 nvs_flash_init() 的错误码
 */
esp_err_t settings_init(void);

/**
 * @brief 读取保存的闪烁周期
 *
 * @param fallback 没有保存过时的返回值。调用方传入自己的默认值，
 *                 这样"第一次开机"和"保存过"两种情况可以走同一段代码
 * @return 保存过的周期（毫秒）；没保存过或读取失败时返回 fallback
 */
uint32_t settings_get_period_ms(uint32_t fallback);

/**
 * @brief 保存闪烁周期
 *
 * 每次调用都会产生一次 Flash 写入。适合"用户操作后保存"，
 * 不适合放进循环。
 *
 * @param period_ms 要保存的周期
 * @return
 *      - ESP_OK  成功（已落盘）
 *      - 其他    透传 NVS 的错误码
 */
esp_err_t settings_set_period_ms(uint32_t period_ms);

#ifdef __cplusplus
}
#endif
