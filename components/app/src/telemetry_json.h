/*
 * telemetry_json.h —— 把一次采样记录组装成一行上报报文（app 组件私有）
 *
 * 这个文件放 src/ 而不是 include/，含义跟 app_internal.h 一样：
 * "只给本组件自己的 .c 用"，别的组件 #include 不到。
 * 靠 CMakeLists 的 PRIV_INCLUDE_DIRS "src" 生效。
 *
 * ── 为什么把"报文"单独拆成一个模块 ──────────────────────────────
 *
 * 如果格式化和采集写在一起，报文的样子就散落在任务循环里，
 * 一旦有两个输出端（串口 + MQTT）就得把那段 snprintf 抄一遍，
 * 再手工保持两处格式一致——字段一改，总有一个人会漏改。
 *
 * 现在把"读一次传感器"（sensor_task.c）和"读出来的值怎么变成字节"
 * （本模块）分开：
 *   - 采集那边只管拿数据，不关心 JSON 语法；
 *   - 本模块只管拼字符串，不关心数据从哪来。
 * 串口和 MQTT 两个消费者调的是**同一个**函数，所以两边打出来的
 * 报文**逐字节相同**——这本身就是一条有用的性质：对不上就说明有问题。
 *
 * 这是"单一职责"在嵌入式里最实用的一种切法——按**变化的原因**切：
 * 传感器驱动会因为换型号而变，报文格式会因为换协议而变，
 * 两者变化的理由不同，就不该待在同一个函数里。
 *
 * ── 为什么"取环境信息"也放在本模块 ──────────────────────────────
 *
 * 报文里除了采样值，还有几个"发出时刻的环境事实"：
 *   wifi   —— 当前连没连上
 *   rssi   —— 当前信号强度
 *   clock  —— 时间戳是 Unix 基准还是开机基准
 *
 * 它们**不属于**采样记录（详见 data_model.h 里关于 rssi 的说明），
 * 而是"这条报文被发出去的那一刻"的现场快照，所以由本模块在组装时
 * 现取。这样两个消费者任务各自只需要一行 app_collect_meta()，
 * 不必去调 wifi_mgr / time_mgr，也不会漏掉某个字段。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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
 * @brief 一条报文的"环境信息"部分
 *
 * 与 sensor_data_t 分开：那份是**采样时刻的物理事实**，
 * 这份是**发出时刻的现场状态**。两者时刻不同，语义也不同。
 */
typedef struct {
    /* 发出这条报文时，WiFi 是否已连接并拿到 IP（wifi_mgr_is_connected()） */
    bool    wifi_ok;

    /*
     * RSSI 是否有效。
     *
     * 为什么用一个布尔而不是拿某个数值当哨兵？
     *   因为 **0 dBm 是合法且极强的信号读数**（贴着路由器才有），
     *   用它表示"未知"会把真实数据污染成"没有数据"。
     *   同理也不能用 -127 这种边界值——规范没保证它不会出现。
     *   显式一个布尔位，语义就没有歧义。
     *
     * rssi_valid 为 false 时，报文里 rssi 字段输出 **null**（不是 0）。
     */
    bool    rssi_valid;
    int8_t  rssi;         /*!< dBm，仅 rssi_valid 为真时有意义 */

    /*
     * 时间戳，已换算。ts_is_unix 为真时是 Unix 毫秒，否则是开机毫秒。
     *
     * 为什么要带一个"基准"标志，而不是让接收端按数值大小猜？
     *   因为"靠阈值区分"是典型的隐式约定：设备连续开机 55 年后
     *   开机毫秒也会涨到 1.7e12 量级，判据就失效了。而且这类猜测
     *   一旦写错，表现是"时间对不上"，很难回溯到"阈值取错了"。
     *   报文里恒发一个 clock 字段明说，代价约 22 字节，
     *   换来的是接收端不需要任何启发式规则。
     */
    int64_t ts_ms;
    bool    ts_is_unix;
} telemetry_meta_t;

/**
 * @brief 采集当前的环境信息，填满 meta
 *
 * 内部会查 wifi_mgr（连接状态 + RSSI）和 time_mgr（时间基准），
 * 并把 boot_ms 换算成 Unix 毫秒（未同步时原样返回）。
 *
 * @param meta    输出。必须非 NULL
 * @param boot_ms 这条记录的采样时刻（开机毫秒），
 *                即 sensor_data_t.timestamp_ms
 */
void app_collect_meta(telemetry_meta_t *meta, int64_t boot_ms);

/**
 * @brief 把一条采样记录 + 环境信息格式化成一行 JSON
 *
 * 输出是紧凑格式（没有多余空格），字段命名对齐文档 §14：
 *
 *   {"device_id":"esp32s3_001","ts":1789000000000,"seq":1,
 *    "temperature_c":26,"humidity_rh":58,
 *    "wifi":true,"rssi":-47,"clock":"unix"}
 *
 * 越短越省流量，也越好在串口一行里看全。
 * 未连接时 rssi 输出 null（不是 0，理由见 telemetry_meta_t 的说明）。
 *
 * @param buf  输出缓冲。建议至少 192 字节（当前最长约 163）
 * @param len  缓冲长度
 * @param d    采样记录
 * @param meta 环境信息
 * @return 写入的字符数（不含结尾 '\0'）；缓冲不够或参数非法时返回 -1
 */
int app_format_reading_json(char *buf, size_t len, const sensor_data_t *d,
                            const telemetry_meta_t *meta);
