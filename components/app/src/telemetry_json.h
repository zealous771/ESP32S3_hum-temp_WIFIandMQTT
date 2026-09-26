/*
 * telemetry_json.h —— 遥测报文的格式化（app 组件私有）
 *
 * 这个文件放 src/ 而不是 include/，含义跟 app_internal.h 一样：
 * "只给本组件自己的 .c 用"，别的组件 #include 不到。
 * 靠 CMakeLists 的 PRIV_INCLUDE_DIRS "src" 生效。
 *
 * 为什么把"报文长什么样"单独拆成一个模块？
 *
 *   如果格式化和采集写在一起，报文的样子就散落在任务循环里，
 *   下一轮要接 MQTT 时得先把那段 snprintf 抄一遍，再手工保持
 *   两处格式一致——一旦字段改了，总有一个人会漏改。
 *
 *   现在把"读一次传感器"（telemetry_task.c）和"读出来的值怎么
 *   变成字节"（本模块）分开：
 *     - 采集那边只管拿数据，不关心 JSON 语法；
 *     - 格式化这边只管拼字符串，不关心数据从哪来。
 *   接 MQTT 时只是多一个调用方，报文格式一处修改、处处生效。
 *
 *   这是"单一职责"在嵌入式里最实用的一种切法——按**变化的原因**
 *   切：传感器驱动会因为换型号而变，报文格式会因为换协议而变，
 *   两者变化的理由不同，就不该待在同一个函数里。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

/*
 * 入参类型是 sensor_data_t，不是 dht11_reading_t。
 *
 * 这个改动本身就是分层在起作用：本模块只负责"把系统内部的记录
 * 变成字节"，它不应该知道数据是从哪颗传感器来的。改完之后，
 * 换传感器（dht11 → sht30）不会碰到这个文件一个字，
 * 换传输协议（串口 → MQTT）也不会。
 */
#include "data_model.h"

/**
 * @brief 把一条采样记录格式化成一行 JSON
 *
 * 为什么是独立函数而不是写在任务里？
 *   下一轮要接 MQTT，届时这份报文要直接 publish 出去。
 *   把"报文长什么样"集中在一处，接 MQTT 时只是多一个调用方，
 *   格式改一处、处处生效。
 *
 * 输出是紧凑格式（没有多余空格），字段命名对齐文档 §14：
 *   {"device_id":"esp32s3_001","ts":123456,"seq":1,
 *    "temperature_c":26,"humidity_rh":58,"wifi":true}
 * 越短越省流量，也越好在串口一行里看全。
 *
 * 与文档 §14 的差异只有一处：**暂时没有 rssi 字段**。
 * rssi 需要 wifi_mgr 先暴露一个 RSSI 查询接口，留到接 MQTT
 * 那一步和它一起加，避免本阶段引入额外的网络相关依赖。
 *
 * @param buf      输出缓冲
 * @param len      缓冲长度
 * @param d        采样记录
 * @param wifi_ok  当前是否已联网（由调用方查，格式化函数不碰网络）
 * @return 写入的字符数（不含结尾 '\0'）；缓冲不够时返回 -1
 */
int app_format_reading_json(char *buf, size_t len, const sensor_data_t *d, bool wifi_ok);
