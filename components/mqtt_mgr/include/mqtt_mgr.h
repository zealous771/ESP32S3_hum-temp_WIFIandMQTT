/*
 * mqtt_mgr.h —— MQTT 客户端的公开接口
 *
 * 这个组件回答一个问题：**怎么把一条消息送到 broker，并且知道现在通不通？**
 *
 * ── 为什么独立成一个组件 ────────────────────────────────────────
 *
 * 与 wifi_mgr 同一条理由：把"传输"这件事从"业务"里切出去。
 *
 *   mqtt_mgr      连接管理：连、断线重连、状态查询、publish
 *   app/mqtt_task 业务：从队列取数据、组报文、决定发不发
 *
 * 切开的收益是可见的：topic 怎么拼、LWT 怎么设、重连多久一次，
 * 这些都是"传输层"的决定，集中在这里；而"什么时候发、发什么内容"
 * 是业务决定，在 mqtt_task 里。换 broker、换 QoS 策略不用碰任务代码，
 * 改任务逻辑也不用担心把连接搞坏。
 *
 * ── 对外形状与 wifi_mgr 刻意保持一致 ────────────────────────────
 *
 *   mqtt_mgr_init()            启动（**不阻塞**）
 *   mqtt_mgr_wait_connected()  需要时等结果（带超时）
 *   mqtt_mgr_is_connected()    随时查询，给不阻塞的任务用
 *
 * 三件套的形状一样，用的人不需要记两套模式。这跟 HAL 里
 * "每个外设都是 Init/DeInit/读写"是同一种一致性红利。
 *
 * ── Topic 方案集中在这里，调用方写不错 ──────────────────────────
 *
 * 文档 §13 定的方案是 `iot/{device_id}/...`。本组件把前缀
 * （`iot/<device_id>/`）在 init 时拼好，对外只暴露**后缀**：
 *
 *   mqtt_mgr_publish("telemetry", json, len, 1, false);
 *                 → iot/esp32s3_001/telemetry
 *
 * 为什么不让调用方传完整 topic？因为它一定会写错——少个斜杠、
 * device_id 拼错大小写、把 retain 和 qos 弄反。让 API 只接受后缀，
 * 把"前缀长什么样""QoS 该用几""要不要 retain"这些决定收进组件，
 * 调用方就没有犯错的空间。
 *
 * ── 在线状态与 LWT（文档 §17）────────────────────────────────────
 *
 *   init 时通过 `session.last_will` 设置**遗嘱消息**：
 *     topic  iot/<device_id>/status
 *     payload {"state":"offline"}
 *     QoS 1 + retain
 *   这条消息由 broker 保管，**设备异常掉线时由 broker 代发**
 *   （掉电、断网、任务卡死都算，只要 TCP 连接被 broker 判定断开）。
 *
 *   连接成功时本组件主动发 {"state":"online"}（同样 QoS1 + retain）。
 *
 *   于是后端只需要订阅 status 这一个 topic，就能知道设备在不在线——
 *   不需要自己维护"多久没收到数据就认为掉线"的超时逻辑。
 *   这是 MQTT 相比 HTTP 上报最实质的一个优势。
 *
 * ── 行为约定一：空 URI 不发散 ───────────────────────────────────
 *
 *   config->uri 传 NULL 或空串时，mqtt_mgr_init() 打一条 ESP_LOGW
 *   说明"未配置 broker，不启用上报"，然后返回 ESP_ERR_INVALID_ARG。
 *   调用方应当把这**一种** ESP_ERR_INVALID_ARG 理解为"不启用 MQTT"，
 *   而不是致命错误——设备照常采集、照常往串口打报文，只是不发出去。
 *
 *   与 wifi_mgr 的空 SSID、time_mgr 的空 NTP 服务器是同一个模式：
 *   **让"还没配好"能正常编译、烧录、运行**，而不是一上电就崩。
 *
 * ── 行为约定二：init 不阻塞 ─────────────────────────────────────
 *
 *   init 只做"建事件组 + 配置客户端 + 注册回调 + 启动"，然后立刻返回。
 *   真正连上 broker 是几秒之后由事件通知的。要不要等、等多久，
 *   由调用方用 mqtt_mgr_wait_connected() 决定。
 *
 * ── 行为约定三：重连交给 esp-mqtt，本组件不接管 WiFi 事件 ───────
 *
 *   esp_mqtt_client 自带自动重连（`network.disable_auto_reconnect = false`），
 *   WiFi 恢复后下一次重试就会成功。本组件**不去监听** WiFi 的
 *   GOT_IP/DISCONNECTED 事件来主动重连——那样等于自己又实现一套状态机，
 *   与 esp-mqtt 内部那套并存，两者会在边界情况下打架（典型的竞态来源）。
 *   一个状态机总比两个好。
 *
 *   代价是 WiFi 断开期间 MQTT 仍会按 reconnect_timeout_ms 定期重试并
 *   打日志，所以本组件对断线日志做了限速（前 3 次全打，之后每 10 次一条），
 *   与 wifi_mgr 的做法一致。
 *
 * ── 行为约定四：未连接时 publish 直接失败，不入 outbox ──────────
 *
 *   esp-mqtt 的 `esp_mqtt_client_publish()` 在未连接时**不会报错**：
 *   QoS1 的消息会进内部 outbox 排队，30 秒后静默过期丢弃
 *   （除非打开 CONFIG_MQTT_SKIP_PUBLISH_IF_DISCONNECTED）。
 *   这个行为对本工程是危险的——"发出去了"和"丢了"在返回值上分不出来。
 *
 *   所以本组件在 publish 入口先查连接状态，未连接直接返回
 *   ESP_ERR_INVALID_STATE，**根本不碰 esp-mqtt 的 publish**。
 *   调用方拿到明确的失败，可以自己计数、自己决定怎么办。
 *
 * 线程安全：is_connected() / state_str() 可以从任意任务调用；
 *          publish() 可以从任意任务调用（内部有 esp-mqtt 自己的锁）；
 *          init() / wait_connected() 只在启动阶段由同一个任务调用。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 后缀常量。用宏而不是让调用方手写字符串字面量，避免拼错——
 * 拼错的 topic 不会报任何错，只是后端永远收不到，很难查。
 */
#define MQTT_MGR_SUFFIX_TELEMETRY   "telemetry"
#define MQTT_MGR_SUFFIX_STATUS      "status"

/** MQTT 连接配置 */
typedef struct {
    /*
     * broker 地址，完整 URI，例如 "mqtt://192.168.31.128:1883"。
     *
     * **不要填 localhost 或 127.0.0.1**——那是设备自己，不是你的电脑。
     * 要填运行 broker 那台机器在局域网里的地址。
     * 空串表示不启用 MQTT（见文件头约定一）。
     */
    const char *uri;

    /*
     * 设备标识。用于拼 topic 前缀（iot/<device_id>/...）和默认 client_id。
     * 同一批设备**不能重名**，否则后端的时序数据会混在一起，
     * 而且两个设备用同一个 client_id 连同一个 broker 会互相把对方踢下线。
     */
    const char *device_id;

    /* 用户名 / 密码。NULL 或空串表示匿名连接（开发阶段用 Mosquitto 默认配置） */
    const char *username;
    const char *password;

    /*
     * client_id。传 NULL 则用 device_id。
     *
     * 为什么单独留一个字段：MQTT 规范要求 client_id 在 broker 上唯一，
     * 而"设备标识"和"连接标识"语义上并不一定相同——
     * 比如同一台设备跑两份固件做对比测试时，需要不同的 client_id
     * 但相同的 device_id（数据要归到同一台设备名下）。
     */
    const char *client_id;

    /*
     * 心跳间隔，秒。
     *
     * 作用有两个：让 broker 知道设备还活着（否则会被判定超时踢掉），
     * 以及**探测假死连接**——AP 还在但上行链路断了时，TCP 不会立刻报错，
     * 要等 keepalive×1.5 才发现。
     *
     * 所以这个值不能太大（假死发现得慢），也不能太小（流量与日志噪音）。
     * 60 秒是常见折中；低于 30 秒在 2 秒一次的上报节奏下没有额外收益。
     */
    uint32_t keepalive_s;

    /* 断线后的重试间隔，毫秒。太小会刷日志，太大会让恢复变慢 */
    uint32_t reconnect_timeout_ms;

    /* 单次网络操作（连接、读写）的超时，毫秒 */
    uint32_t net_timeout_ms;
} mqtt_mgr_config_t;

/**
 * @brief 初始化并启动 MQTT 客户端（不阻塞，不等连接）
 *
 * 调用前必须先让网络就绪（默认事件循环、默认网络接口都已创建），
 * 也就是要在 wifi_mgr_init() 成功之后调用。**不要求已经拿到 IP**——
 * esp-mqtt 会自己重试，等网络通了自然就连上。
 *
 * @note 关于字符串的生命周期：esp-mqtt **不保证**复制配置里的所有字符串。
 *       实测（mqtt_client.c）：URI、username、password、LWT 的 payload 会被
 *       strdup 或 memcpy，但 **LWT 的 topic 只存指针不复制**。
 *       所以本组件的约定是：**传进来的字符串必须全程存活**。
 *       本工程传的都是 Kconfig 生成的字符串字面量，天然满足。
 *       如果哪天要从 NVS 读出 broker 地址再传进来，必须自己保证那块内存
 *       活到客户端销毁为止——否则症状是"连接时发出去的遗嘱 topic 是垃圾"，
 *       极难排查。
 *
 * @param config 配置。函数内只读取，但其中的字符串指针会被 esp-mqtt
 *               长期持有，见上面的 @note
 * @return
 *      - ESP_OK                 已启动；连接结果稍后由事件通知
 *      - ESP_ERR_INVALID_ARG    config 为 NULL、device_id 为 NULL，
 *                               或 uri 为空（**表示不启用 MQTT**，见约定一），
 *                               或 device_id 长到 topic 前缀放不下
 *      - ESP_ERR_INVALID_STATE  已经初始化过了（本组件只支持初始化一次）
 *      - ESP_ERR_NO_MEM         事件组创建失败，或 esp_mqtt_client_init() 返回 NULL
 */
esp_err_t mqtt_mgr_init(const mqtt_mgr_config_t *config);

/**
 * @brief 阻塞等待连接到 broker
 *
 * @note 超时换算的坑同 wifi_mgr_wait_connected()：pdMS_TO_TICKS() 向下取整，
 *       本工程 tick 频率 100Hz（1 tick = 10ms），所以传 1~9 会算成 0 个 tick，
 *       等于"不等待、立刻超时"，而且不会报任何错。
 *
 * @param timeout_ms 最长等待毫秒数。传 0 表示立即返回（不等待）
 * @return
 *      - ESP_OK                 已连接
 *      - ESP_ERR_TIMEOUT        超时仍未连接（broker 没起、地址填错、
 *                               网络不通，或断线后还在重试）
 *      - ESP_ERR_INVALID_STATE  尚未成功调用 mqtt_mgr_init()
 */
esp_err_t mqtt_mgr_wait_connected(uint32_t timeout_ms);

/**
 * @brief 查询当前是否已连接到 broker
 *
 * 判定依据是 esp-mqtt 的 MQTT_EVENT_CONNECTED / DISCONNECTED 事件，
 * 与 wait_connected() 读的是同一个事件组，所以两者不会给出矛盾的结论。
 *
 * @return true 已连接；false 未初始化、未启用，或当前未连接
 */
bool mqtt_mgr_is_connected(void);

/**
 * @brief 取当前连接状态的文字描述，用于日志与监控
 *
 * @return "disabled"（未配置 broker）/ "connected" / "disconnected"
 */
const char *mqtt_mgr_state_str(void);

/**
 * @brief 发布一条消息到 `iot/<device_id>/<suffix>`
 *
 * **未连接时直接返回失败，不把消息塞进 esp-mqtt 的 outbox**
 * （理由见文件头约定四）。
 *
 * @param suffix  topic 后缀，用 MQTT_MGR_SUFFIX_* 宏。不能含 '/' 开头
 * @param payload 报文内容（不要求以 '\0' 结尾，长度由 len 决定）
 * @param len     报文长度；传 0 表示用 strlen(payload)
 * @param qos     QoS 等级（0/1/2）。本工程 telemetry 用 1
 * @param retain  是否保留。status 用 true（新订阅者立刻能看到最后状态），
 *                telemetry 用 false（历史数据没有"保留"的意义）
 * @return
 *      - ESP_OK                 已交给 esp-mqtt 发送（QoS1 时是"已入队"，
 *                               不等于 broker 已确认收到）
 *      - ESP_ERR_INVALID_ARG    suffix 为 NULL/空，或拼出的 topic 太长
 *      - ESP_ERR_INVALID_STATE  未初始化、未启用，或**当前未连接**
 *      - ESP_ERR_NO_MEM         esp-mqtt 的 outbox 已满（publish 返回 -2）
 *      - ESP_FAIL               其他发送失败（publish 返回 -1）
 */
esp_err_t mqtt_mgr_publish(const char *suffix, const char *payload,
                           int len, int qos, bool retain);

#ifdef __cplusplus
}
#endif
