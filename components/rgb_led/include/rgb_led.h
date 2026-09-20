/*
 * rgb_led.h —— 板载可寻址 RGB（WS2812B）的封装
 *
 * 为什么它不能和普通 LED 共用 led 组件？
 *
 *   普通 LED 就是一根 GPIO：写 0 或 1，电平立刻改变，就这么简单。
 *
 *   可寻址 RGB（WS2812B 一类）完全不同——它是一根**单线串行总线**，
 *   靠"高电平持续多久"来编码 0 和 1：
 *
 *       编码 0：高电平约 0.4 us，随后低电平约 0.85 us
 *       编码 1：高电平约 0.8 us，随后低电平约 0.45 us
 *       复位：  低电平持续 50 us 以上，灯才会锁存并显示
 *
 *   每颗灯 24 位（GRB 各 8 位），时序容差在 ±150 ns 量级。
 *   靠软件翻转 GPIO 做不到——一次中断延迟就把波形毁了，
 *   表现出来就是颜色乱跳或者干脆不亮。
 *
 *   所以必须用硬件外设产生这个波形。ESP32-S3 上有两条路：
 *     - RMT（Remote Control Transceiver）—— 本组件用的方案
 *     - SPI —— 把每个 bit 展开成一个字节，借 SPI 时钟凑出波形
 *   RMT 本来就是为红外遥控、单总线协议这类"精确波形"设计的，
 *   直接给出"这一段高电平多少纳秒、那一段低电平多少纳秒"，
 *   比 SPI 那套取巧做法更贴合问题本身。
 *
 * 依赖从哪来？
 *
 *   本组件没有自己实现 WS2812B 的时序编码，而是用了 Espressif 官方的
 *   espressif/led_strip 组件。它不是 IDF 自带的，通过同目录下的
 *   idf_component.yml 声明依赖，构建时由**组件管理器**自动下载到
 *   工程根目录的 managed_components/ 下。
 *
 *   这就是 ESP-IDF 的包管理方式，对照 HAL 时代：
 *     HAL 时代  —— 手动拷源码进工程，或者加一个 .a 静态库
 *     ESP-IDF   —— 在 yml 里写一行版本号，其余交给构建系统
 *   好处是版本关系明确（dependencies.lock 会锁死实际用的版本），
 *   换机器构建结果一致；坏处是首次构建需要联网。
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化可寻址 RGB
 *
 * @param gpio_num  数据线所在的 GPIO
 * @param led_count 灯珠数量。板载那颗 RGB 是 1
 * @return
 *      - ESP_OK               成功
 *      - ESP_ERR_INVALID_ARG  参数非法
 *      - 其他                 透传底层 RMT 驱动的错误码
 */
esp_err_t rgb_led_init(int gpio_num, uint32_t led_count);

/**
 * @brief 设置某颗灯的颜色并立即刷新
 *
 * 内部是"设置像素 + 推送"两步。单灯场景合并成一次调用更顺手；
 * 如果要驱动长灯带并且需要一次推送多颗灯的颜色，那应该拆开，
 * 否则每设一颗就产生一次复位脉冲，灯带会闪。
 *
 * @param index 灯珠序号，从 0 开始
 * @param r/g/b 各通道亮度 0~255
 */
esp_err_t rgb_led_set(uint32_t index, uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief 用 HSV 设置颜色并立即刷新
 *
 * 调颜色时 HSV 比 RGB 直观得多——想换个色相只改 H，
 * 想调浓淡只改 S，想调明暗只改 V，三者互不干扰。
 * RGB 里改一个分量，色相和亮度会一起变。
 *
 * @param index 灯珠序号
 * @param h 色相 0~360（0=红 120=绿 240=蓝，超出会取模）
 * @param s 饱和度 0~100（0=白）
 * @param v 明度 0~100（0=灭）
 */
esp_err_t rgb_led_set_hsv(uint32_t index, uint32_t h, uint32_t s, uint32_t v);

/**
 * @brief 熄灭所有灯并刷新
 */
esp_err_t rgb_led_clear(void);

/**
 * @brief 已初始化的灯珠数量；未初始化返回 0
 */
uint32_t rgb_led_count(void);

#ifdef __cplusplus
}
#endif
