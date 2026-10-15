/*
 * wifi_mgr.h —— WiFi Station（STA）的公开接口
 *
 * 这个组件回答一个问题：ESP32-S3 怎么连上路由器？
 *
 * HAL 里"联网"通常意味着外挂一个模块，用一串 AT 指令去驱动它；
 * ESP32-S3 的 WiFi 是片内的，ESP-IDF 把它拆成了三层：
 *
 *   esp_wifi   —— 协议栈与驱动：扫描、认证、连接、断线重连
 *   esp_netif  —— 把 WiFi 接口接到 TCP/IP 协议栈（lwIP）上，
 *                 并负责用 DHCP 拿 IP
 *   esp_event  —— 事件总线：协议栈"有事发生"时往这里投递事件，
 *                 应用层注册回调去处理，而不是自己轮询
 *
 * 关键的观念转变是：这三层不是"调用完就完事"，而是"注册回调 + 等事件"。
 *
 *   HAL 里：  HAL_XXX_Init() 返回时，外设就绪。
 *   这里：    esp_wifi_start() 返回时只是"驱动开跑了"，
 *             真正"连上并拿到 IP"是几秒之后由事件通知的。
 *
 * 所以本组件对外只暴露三件事：
 *   wifi_mgr_init()            把三层串起来并启动（**不阻塞**）
 *   wifi_mgr_wait_connected()  需要联网时才等结果（带超时）
 *   wifi_mgr_is_connected()    随时查询，给不阻塞的任务用
 *
 * ── 行为约定一：空 SSID 不发散（离线运行是被支持的） ──────────────
 *
 *   config->ssid 传 NULL 或空串时，wifi_mgr_init() 会打一条 ESP_LOGW
 *   说明"没有配置 SSID，不联网"，然后返回 ESP_ERR_INVALID_ARG。
 *   调用方应当把**这一种** ESP_ERR_INVALID_ARG 理解为"不联网"，
 *   而不是致命错误——尤其不要写成 ESP_ERROR_CHECK(wifi_mgr_init(...))，
 *   那样一上电就 abort。
 *
 *   为什么允许空 SSID？本工程的凭据来自 Kconfig，而 WIFI_SSID 默认
 *   就是空串（凭据只存本机，不入库）。空的含义是"这台机器还没填路由器
 *   凭据"，此时设备照常跑 LED、按键、RGB、温湿度采集，只是不联网。
 *   让"还没填密码"能正常编译、烧录、运行，比让它上电就崩掉友好得多。
 *   （main/Kconfig.projbuild 与 main.c 里各有一道编译期提醒配合这件事。）
 *
 * ── 行为约定二：wifi_mgr_init() 不阻塞 ────────────────────────────
 *
 *   init 只做"注册回调 + 启动驱动"，然后立刻返回；要不要等、等多久，
 *   由调用方用 wifi_mgr_wait_connected() 决定。
 *
 *   这是刻意的。路由器不在、密码错、信号差，都会让"连上并拿到 IP"
 *   花掉几秒到几十秒；如果 init 里直接阻塞着等，WiFi 一慢整个应用启动
 *   就被卡住——按键不响应、LED 不闪、传感器不采。
 *   把"等待"从"初始化"里拆出来，最坏情况也只是少一次联网，
 *   本地功能一个都不受影响。
 *
 * ── 行为约定三：重连永不停止，只对日志限速 ────────────────────────
 *
 *   断线后自动重连，而且**一直重连下去，不设次数上限**。
 *   路由器开机时不在、路由重启、AP 断电，设备都会自己恢复，
 *   不需要人工复位。
 *
 *   节流的只是**日志**：前 3 次断线每次都打（开机阶段的前几次最有诊断
 *   价值），之后每 10 次打一条并带累计次数。这样路由器长时间不可达时，
 *   串口不会被同一条日志淹没。
 *
 *   为什么不是"失败 N 次就停手"？因为那是拿"不可恢复的状态"去换"日志
 *   干净"——一旦在启动阶段用完重试预算，设备就永久失去联网能力，
 *   对常驻的传感节点来说是很糟的失败模式。刷屏的问题就只解决刷屏。
 *   每次成功拿到 IP 后计数清零，下一次故障又恢复全速打印。
 *
 * 线程安全：wifi_mgr_is_connected() 可以从任意任务调用；
 *          wifi_mgr_init() / wifi_mgr_wait_connected() 只在启动阶段
 *          由同一个任务调用，内部不加互斥保护。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 关于 connect_timeout_ms：
 *
 *   它是"等待拿到 IP"的默认超时，在 wifi_mgr_init() 里存下来，
 *   供 wifi_mgr_wait_connected(0) 使用（传 0 = 用这里配置的值）。
 *   调用方当然也可以不用它，直接给 wait_connected() 传自己的超时——
 *   两种写法都支持，因为"等多久"是应用决策，不该由驱动层写死。
 */
/** WiFi 连接配置 */
typedef struct {
    const char *ssid;                /*!< 目标 AP 名称，最长 32 字节；空串表示不联网 */
    const char *password;            /*!< 密码；开放网络传空串 */
    uint32_t    connect_timeout_ms;  /*!< 等待拿到 IP 的超时，毫秒 */
} wifi_mgr_config_t;

/**
 * @brief 初始化 WiFi 并启动 Station 模式（不阻塞，不等连接）
 *
 * 调用分两段，原则是**先校验、后动手**：
 *
 *   第 0 段：参数校验。此时一行 ESP-IDF 接口都还没碰。
 *     - 已初始化过 → ESP_ERR_INVALID_STATE
 *     - config 为 NULL / SSID 为空 / SSID 超长 / 密码为 NULL 或超长
 *       → ESP_ERR_INVALID_ARG
 *
 *     校验全部前置是刻意的：空 SSID 是"不联网、照常开机"这条受支持路径
 *     的信号，必须在网络接口层、默认事件循环、WiFi 驱动都还没被初始化时
 *     就返回。否则这条路径会留下"初始化过却从未 start"的半成品状态
 *     （还占着默认事件循环和一个 netif），比"什么都没碰"难推理得多。
 *
 *   第 1 段：按顺序完成下列步骤，顺序不能改，每一步都依赖前一步：
 *
 *   1. 创建事件组（必须在注册回调**之前**，否则回调里无处设置标志位）
 *   2. esp_netif_init()                      初始化网络接口层
 *   3. esp_event_loop_create_default()       建立默认事件循环
 *   4. esp_netif_create_default_wifi_sta()   创建默认 STA 网络接口
 *   5. esp_wifi_init()                       初始化 WiFi 驱动
 *   6. 注册 WIFI_EVENT / IP_EVENT 两个回调，并保存实例句柄
 *   7. esp_wifi_set_mode(WIFI_MODE_STA)
 *   8. esp_wifi_set_config(WIFI_IF_STA, ...) 填入 SSID / 密码
 *   9. esp_wifi_start()                      启动驱动
 *
 * 注意第 9 步之后**不会**直接调 esp_wifi_connect()：驱动真正就绪是
 * 异步的，完成后会发出 WIFI_EVENT_STA_START，连接动作在那个回调里发起。
 * 这是官方 station 示例的做法，可以避开"驱动没就绪就发起连接"的竞态。
 *
 * 本函数返回时**可能还没连上**（正常现象，见文件头的约定二）。
 *
 * @param config 配置。SSID / 密码会被拷进驱动自己的缓冲区，
 *               所以调用方不必保持这两个字符串的生命周期
 * @return
 *      - ESP_OK                 已启动；连接结果稍后由事件通知
 *      - ESP_ERR_INVALID_ARG    config 为 NULL；SSID 为空
 *                               （**表示不联网**，见文件头的约定一）；
 *                               SSID 长于 32 字节；密码长于 64 字节；
 *                               password 为 NULL（开放网络请传空串）
 *      - ESP_ERR_INVALID_STATE  已经初始化过了（本组件只支持初始化一次）
 *      - ESP_ERR_NO_MEM         事件组或默认 STA 网络接口创建失败
 *      - 其他                   透传 esp_netif / esp_event / esp_wifi 的错误码
 */
esp_err_t wifi_mgr_init(const wifi_mgr_config_t *config);

/**
 * @brief 阻塞等待拿到 IP
 *
 * 内部用 xEventGroupWaitBits() 等 IP_EVENT_STA_GOT_IP 设置的那个标志位。
 * 适合在"必须联网才能干活"的任务里调用（比如上报数据的任务）；启动路径上
 * 也可以调用，但要用有界超时，避免路由器不可达时拖慢启动。
 *
 * 本函数与 wifi_mgr_is_connected() 读的是**同一个**标志位，
 * 所以两者不会给出互相矛盾的结论。
 *
 * @note 超时换算的坑：pdMS_TO_TICKS() 是向下取整的整数除法，
 *       本工程 tick 频率 100Hz（CONFIG_FREERTOS_HZ=100），
 *       即 1 tick = 10ms。所以传 1~9 会被算成 0 个 tick，
 *       等于"不等待、立刻返回超时"，而且**不会报任何错**。
 *
 * @param timeout_ms 最长等待毫秒数；传 0 表示改用 wifi_mgr_init()
 *                   时配置的 connect_timeout_ms（两者都是 0 则立即超时）
 * @return
 *      - ESP_OK                 已经拿到 IP
 *      - ESP_ERR_TIMEOUT        超时仍未拿到 IP（路由器不可达、密码错，
 *                               或断线后一直在重连但还没拿到 IP，都会走到这里）
 *      - ESP_ERR_INVALID_STATE  尚未成功调用 wifi_mgr_init()
 *                               （或上次初始化失败回滚过，没有可等待的事件组）
 */
esp_err_t wifi_mgr_wait_connected(uint32_t timeout_ms);

/**
 * @brief 查询当前是否已连接并拿到 IP
 *
 * 读的是事件组里那一位，和 wifi_mgr_wait_connected() 等的是同一个来源。
 * 可以在任意任务里调用；未初始化、正在重连、断线后尚未重新拿到 IP 时
 * 都返回 false。
 *
 * 判定依据是"拿到 IP"（IP_EVENT_STA_GOT_IP）而不是"链路连上"
 * （WIFI_EVENT_STA_CONNECTED）：前者才代表真的能收发数据了。
 *
 * @return true 已连接且已拿到 IP；false 未初始化或尚未拿到 IP
 */
bool wifi_mgr_is_connected(void);

/**
 * @brief 读取当前 AP 的信号强度
 *
 * 数据来源是 **WiFi 驱动自己维护的连接记录**（`esp_wifi_sta_get_ap_info()`），
 * 不是本次事件里的快照——所以任意时刻读到的都是"此刻"的值，而不是
 * "上次断线时"的旧值。
 *
 * 不需要网络接口句柄：RSSI 是链路层信息，驱动直接就能回答，
 * 与 lwIP / DHCP 那一层无关。
 *
 * @note 返回值有符号，单位 dBm，**越接近 0 越强**：
 *         -30 极强（贴着路由器）、-67 良好、-80 偏弱、-90 边缘。
 *       注意 0 是一个**合法且极强**的读数，不是"未知"——
 *       调用方不能用 0 当"没读到"的哨兵，必须看本函数的返回值。
 *
 * @param rssi 输出。函数成功返回时才会被写入；失败时内容未定义
 * @return
 *      - ESP_OK                  成功
 *      - ESP_ERR_INVALID_ARG     rssi 为 NULL
 *      - ESP_ERR_WIFI_NOT_CONNECT 尚未初始化，或当前没连上 AP
 *                                （两者都归到这里：对调用方来说
 *                                 "读不到 RSSI"是同一件事）
 *      - 其他                    透传 esp_wifi_sta_get_ap_info() 的错误码
 */
esp_err_t wifi_mgr_get_rssi(int8_t *rssi);

#ifdef __cplusplus
}
#endif
