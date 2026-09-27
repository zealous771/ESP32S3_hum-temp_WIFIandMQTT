/*
 * mqtt_mgr.c —— MQTT 客户端的实现
 *
 * 实现分三块：
 *   1. init：把配置翻译成 esp_mqtt_client_config_t，建事件组、注册回调、启动
 *   2. 事件回调：维护"连没连上"这一个状态，并在连上时发 online
 *   3. 对外查询/发布：is_connected / state_str / publish
 *
 * ── 状态只有一个来源 ─────────────────────────────────────────────
 *
 * 与 wifi_mgr 一样，用 event group 的 bit0 作为**唯一**的连接状态：
 * wait_connected() 等它、is_connected() 读它，两者不可能矛盾。
 * 不用额外的 bool 变量，因为两个状态变量迟早会不一致。
 *
 * ── 回调跑在哪个任务里（重要）────────────────────────────────────
 *
 * esp-mqtt 内部会创建一个名为 "mqtt_evt" 的事件循环任务
 * （见 esp_mqtt_client_init：未传 event_loop_handle 时它自己建一个）。
 * 用户注册的事件回调是在**那个任务**里执行的，**不是**在持着
 * MQTT_API_LOCK 的 mqtt 任务里。所以回调里可以安全地调用
 * esp_mqtt_client_publish() —— 官方示例就是这么用的
 * （examples/protocols/mqtt/main/app_main.c 在 MQTT_EVENT_CONNECTED 里
 * 直接调 subscribe/unsubscribe）。
 *
 * 但要注意：回调**不是中断上下文**，可以调可能阻塞的 API，
 * 却也不该在里面做重活——它挡着的是 MQTT 事件的处理，做太久会
 * 让后续事件（比如 PUBLISHED 确认）堆积。
 *
 * ── 关于 publish 的返回值（一个必须绕开的坑）────────────────────
 *
 * esp-mqtt 的 esp_mqtt_client_publish() 在**未连接**时不会报错：
 * 它会做 QoS 检查、把消息放进内部 outbox（QoS1/2），然后返回一个
 * 正常的 msg_id（≥0）。消息在 outbox 里最多待 30 秒
 * （CONFIG_MQTT_OUTBOX_EXPIRED_TIMEOUT_MS），然后被静默删除。
 *
 * 也就是说，只看返回值的话，"发到 broker 了"和"进了垃圾桶"完全一样。
 * 这是本组件在 publish 入口先查连接状态的原因（见文件头约定四）：
 * 宁可直接失败，也不要让调用方拿着一个假的成功去计数。
 */
#include "mqtt_mgr.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "mqtt_client.h"

static const char *TAG = "mqtt_mgr";

/*
 * 事件组里"已连接到 broker"那一位（bit0）。理由同 wifi_mgr.c：
 * BIT0 定义在 esp_bit_defs.h，这条包含链上没有头文件会带进来。
 */
#define MQTT_MGR_CONNECTED_BIT ((EventBits_t)0x01)

/*
 * topic 缓冲尺寸。
 *
 *   s_topic_prefix = "iot/<device_id>/"
 *     device_id 上限 31 字节（sensor_data_t.device_id 是 char[32]），
 *     加 "iot/" 和结尾的 '/' 共 36，留到 48 有余量。
 *
 *   s_status_topic = 前缀 + "status"，最长约 43，留到 80。
 *
 * 这两个都是 static：esp-mqtt 的 last_will.topic 存的是**指针**不是副本
 * （见 mqtt_client.c 里 esp_mqtt_set_if_config 的用法），
 * 必须活到客户端销毁为止。用栈上的局部缓冲会留下一根悬空指针，
 * 而且症状是"连接时发送的遗嘱 topic 是垃圾"——非常难查。
 */
#define MQTT_MGR_PREFIX_MAX 48
#define MQTT_MGR_TOPIC_MAX  80

/* 上线/下线的报文（文档 §17）。offline 这份会作为 LWT 交给 broker 保管 */
#define MQTT_MGR_STATUS_ONLINE_JSON  "{\"state\":\"online\"}"
#define MQTT_MGR_STATUS_OFFLINE_JSON "{\"state\":\"offline\"}"

/*
 * 断线日志限速。理由与 wifi_mgr 完全一致：
 * 前 3 次每次打（开机阶段最有诊断价值），之后每 10 次一条带累计次数。
 * 限速的只是**打印**，重连本身由 esp-mqtt 一直进行。
 */
#define MQTT_MGR_LOG_ALWAYS_RETRIES  3
#define MQTT_MGR_LOG_EVERY_N_RETRIES 10

/*
 * esp-mqtt 自己那个任务的栈大小与优先级。
 *
 * 注意这是**协议栈任务**的栈，不是我们 mqtt_task 的栈：收发缓冲、
 * 报文编解码、连接管理都跑在这里。将来上 TLS 要把它调大
 * （TLS 握手会吃好几 KB）。6KB 是 esp-mqtt 的默认值，明文 TCP 够用。
 */
#define MQTT_MGR_TASK_STACK 6144
#define MQTT_MGR_TASK_PRIO  5

static bool s_initialized = false;

/* uri 非空才算"启用"。未启用时所有查询返回"没连上"，publish 直接失败 */
static bool s_enabled = false;

/* 连接状态的唯一来源 */
static EventGroupHandle_t s_event_group = NULL;

static esp_mqtt_client_handle_t s_client = NULL;

/* topic 前缀 "iot/<device_id>/"，init 时拼好 */
static char s_topic_prefix[MQTT_MGR_PREFIX_MAX];

/* LWT 的 topic，init 时拼好。必须是 static，理由见上面的尺寸说明 */
static char s_status_topic[MQTT_MGR_TOPIC_MAX];

/* 本次断线以来累计的断开次数，连上时清零。只用于日志限速 */
static uint32_t s_disconnect_count = 0;

/*
 * 发布一条 status 报文。
 *
 * 只在事件回调里用，所以做成 static 而非对外接口——外部不应该
 * 有机会伪造设备的在线状态。
 */
static void publish_status(const char *json, bool retain)
{
    const int msg_id = esp_mqtt_client_publish(s_client, s_status_topic,
                                               json, 0, 1, retain ? 1 : 0);
    if (msg_id < 0) {
        /* status 是辅助信息，发不出去不影响数据上报，所以只记 WARN */
        ESP_LOGW(TAG, "发布 status 失败（%s）: msg_id=%d", json, msg_id);
    }
}

static void on_mqtt_event(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    /* 注册时没传自定义参数。显式 void 掉，避免"未使用参数"的告警 */
    (void)arg;
    (void)event_base;

    const esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        s_disconnect_count = 0;
        xEventGroupSetBits(s_event_group, MQTT_MGR_CONNECTED_BIT);

        /*
         * 置位**之后**再发 online。
         *
         * 顺序有意：publish_status() 内部不查连接状态（它直接用
         * s_client 发），所以两种顺序都能工作；但先置位能让
         * 其它任务在收到 online 的同一瞬间就能看到 is_connected()==true，
         * 不会出现"broker 已经认为我在线、但本机还说自己没连上"的窗口。
         */
        ESP_LOGI(TAG, "已连接到 broker");
        publish_status(MQTT_MGR_STATUS_ONLINE_JSON, true);
        break;

    case MQTT_EVENT_DISCONNECTED:
        /*
         * 断线属于"辅助功能失败"：本地功能一个都不受影响。
         * 所以清标志位、限速告警，**绝不 abort**。
         * 重连由 esp-mqtt 自己做（disable_auto_reconnect = false）。
         */
        xEventGroupClearBits(s_event_group, MQTT_MGR_CONNECTED_BIT);
        s_disconnect_count++;

        if (s_disconnect_count <= MQTT_MGR_LOG_ALWAYS_RETRIES ||
            (s_disconnect_count % MQTT_MGR_LOG_EVERY_N_RETRIES) == 0) {
            ESP_LOGW(TAG, "与 broker 断开（累计第 %u 次），自动重连中",
                     (unsigned)s_disconnect_count);
        }
        break;

    case MQTT_EVENT_ERROR:
        /*
         * 错误分两类，处理方式完全不同：
         *
         *   CONNECTION_REFUSED —— broker 明确拒绝了连接。最常见的原因是
         *     用户名/密码错（return code 4/5）或 client_id 冲突（2）。
         *     这类问题**重试一万次也没用**，必须人去改配置，
         *     所以用 LOGE 并打印 return code，让排查者一眼看到根因。
         *
         *   其他（传输层/TLS）—— 大多是网络抖动，esp-mqtt 会自己重试，
         *     用 LOGW 就够了，打太响会让日志里 ERROR 贬值。
         */
        if (event != NULL && event->error_handle != NULL) {
            const esp_mqtt_error_codes_t *err = event->error_handle;
            if (err->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
                ESP_LOGE(TAG, "broker 拒绝连接，return code = 0x%02x"
                              "（1=协议版本不支持 2=client_id 被拒 "
                              "4=用户名密码错 5=未授权）",
                         err->connect_return_code);
            } else {
                ESP_LOGW(TAG, "MQTT 错误：type=%d, tls_err=0x%x, sock_errno=%d",
                         err->error_type, err->esp_tls_last_esp_err,
                         err->esp_transport_sock_errno);
            }
        }
        break;

    default:
        /*
         * 其余事件（SUBSCRIBED / PUBLISHED / DATA / BEFORE_CONNECT 等）
         * 本组件当前不关心。显式列一个 default 分支而不是省略：
         * 说明"不处理是有意的"，而不是"忘了"。
         */
        break;
    }
}

esp_err_t mqtt_mgr_init(const mqtt_mgr_config_t *config)
{
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (config == NULL || config->device_id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * 空 URI = 不启用 MQTT，受支持的用法（见头文件约定一）。
     * 与 wifi_mgr 的空 SSID 同一模式：**在碰任何 IDF 接口之前**返回，
     * 免得留下半初始化的状态。
     */
    if (config->uri == NULL || config->uri[0] == '\0') {
        ESP_LOGW(TAG, "未配置 broker 地址，本次不启用 MQTT 上报；"
                      "填法：idf.py menuconfig → MQTT broker");
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * 拼 topic 前缀。用 snprintf 并检查返回值，而不是 strcat：
     * strcat 不检查边界，device_id 一旦超长就是栈/全局溢出。
     * snprintf 返回"本来想写多少"，所以 >= 容量就是被截断了。
     */
    const int n = snprintf(s_topic_prefix, sizeof(s_topic_prefix),
                           "iot/%s/", config->device_id);
    if (n < 0 || n >= (int)sizeof(s_topic_prefix)) {
        ESP_LOGE(TAG, "device_id 太长（%u 字节），topic 前缀放不下",
                 (unsigned)strlen(config->device_id));
        return ESP_ERR_INVALID_ARG;
    }

    const int m = snprintf(s_status_topic, sizeof(s_status_topic), "%s%s",
                           s_topic_prefix, MQTT_MGR_SUFFIX_STATUS);
    if (m < 0 || m >= (int)sizeof(s_status_topic)) {
        ESP_LOGE(TAG, "status topic 放不下");
        return ESP_ERR_INVALID_ARG;
    }

    /* 事件组必须在注册回调**之前**建好，否则回调里无处置位 */
    s_event_group = xEventGroupCreate();
    if (s_event_group == NULL) {
        ESP_LOGE(TAG, "创建事件组失败");
        return ESP_ERR_NO_MEM;
    }

    const esp_mqtt_client_config_t cfg = {
        .broker = {
            .address = { .uri = config->uri },
        },
        .credentials = {
            /* 传 NULL 就用 device_id。config->client_id 的语义见头文件 */
            .client_id = (config->client_id != NULL) ? config->client_id
                                                     : config->device_id,
            /* 空串按"不用"处理：Mosquitto 默认配置是匿名的 */
            .username  = (config->username != NULL && config->username[0] != '\0')
                             ? config->username : NULL,
            .authentication = {
                .password = (config->password != NULL && config->password[0] != '\0')
                                ? config->password : NULL,
            },
        },
        .session = {
            .keepalive = (int)config->keepalive_s,
            /*
             * 遗嘱消息（LWT，文档 §17）。
             *
             * msg_len 传 strlen 而不是 0：传 0 的话 esp-mqtt 会自己
             * strlen 一遍（效果相同），显式给出来是为了表明
             * "这份 payload 没有内嵌 '\0'，长度就是这么多"。
             *
             * QoS 1 + retain：
             *   retain 让**新订阅者**一订阅就能立刻拿到当前状态，
             *   不必等到下一次状态变化——否则后端重启后要等到设备
             *   掉线才知道它曾经在线。
             */
            .last_will = {
                .topic   = s_status_topic,
                .msg     = MQTT_MGR_STATUS_OFFLINE_JSON,
                .msg_len = (int)strlen(MQTT_MGR_STATUS_OFFLINE_JSON),
                .qos     = 1,
                .retain  = 1,
            },
        },
        .network = {
            .reconnect_timeout_ms   = (int)config->reconnect_timeout_ms,
            .timeout_ms             = (int)config->net_timeout_ms,
            /*
             * 自动重连交给 esp-mqtt。本组件不去监听 WiFi 事件主动重连——
             * 两个状态机会在边界情况下打架，见头文件约定三。
             */
            .disable_auto_reconnect = false,
        },
        .task = {
            .stack_size = MQTT_MGR_TASK_STACK,
            .priority   = MQTT_MGR_TASK_PRIO,
        },
    };

    s_client = esp_mqtt_client_init(&cfg);
    if (s_client == NULL) {
        vEventGroupDelete(s_event_group);
        s_event_group = NULL;
        ESP_LOGE(TAG, "创建 MQTT 客户端失败（内存不足？）");
        return ESP_ERR_NO_MEM;
    }

    /*
     * 注册事件回调。用 ESP_EVENT_ANY_ID 拿全部事件，在回调里按
     * event_id 分派——本组件只关心 CONNECTED / DISCONNECTED / ERROR，
     * 其余静默忽略（default 分支里说明了）。
     *
     * 注册失败要回滚：销毁客户端和事件组，让状态回到"什么都没发生"。
     * 不回滚的话，调用方会看到一个"能 publish 但永远连不上"的客户端。
     */
    esp_err_t ret = esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID,
                                                   on_mqtt_event, NULL);
    if (ret != ESP_OK) {
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        vEventGroupDelete(s_event_group);
        s_event_group = NULL;
        ESP_LOGE(TAG, "注册 MQTT 事件回调失败: %s", esp_err_to_name(ret));
        return ret;
    }

    /*
     * start 只表示"开始连接流程"，**不等于已连接**。
     * 连接是异步的，结果由 MQTT_EVENT_CONNECTED 通知。
     */
    ret = esp_mqtt_client_start(s_client);
    if (ret != ESP_OK) {
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        vEventGroupDelete(s_event_group);
        s_event_group = NULL;
        ESP_LOGE(TAG, "启动 MQTT 客户端失败: %s", esp_err_to_name(ret));
        return ret;
    }

    s_initialized = true;
    s_enabled     = true;

    ESP_LOGI(TAG, "MQTT 已启动，broker %s，topic 前缀 %s（连接结果稍后通知）",
             config->uri, s_topic_prefix);
    return ESP_OK;
}

esp_err_t mqtt_mgr_wait_connected(uint32_t timeout_ms)
{
    if (s_event_group == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * xClearOnExit = pdFALSE：拿到标志位后不清零，之后 is_connected()
     * 还能读到"已连接"。与 wifi_mgr_wait_connected 同一写法。
     */
    const EventBits_t bits = xEventGroupWaitBits(s_event_group, MQTT_MGR_CONNECTED_BIT,
                                                pdFALSE, pdFALSE,
                                                pdMS_TO_TICKS(timeout_ms));

    if ((bits & MQTT_MGR_CONNECTED_BIT) != 0) {
        return ESP_OK;
    }

    ESP_LOGW(TAG, "等待 MQTT 连接超时（%u ms）", (unsigned)timeout_ms);
    return ESP_ERR_TIMEOUT;
}

bool mqtt_mgr_is_connected(void)
{
    /* 未初始化时没有事件组可读，直接回答"没连上"，且不打日志（会被频繁调用） */
    if (s_event_group == NULL) {
        return false;
    }

    return (xEventGroupGetBits(s_event_group) & MQTT_MGR_CONNECTED_BIT) != 0;
}

const char *mqtt_mgr_state_str(void)
{
    /*
     * 三态而不是两态，是为了让监控日志能区分两种完全不同的"没连上"：
     *   disabled     —— 配置里没填 broker，这是预期状态，不该报警
     *   disconnected —— 填了但连不上，这才是需要关注的
     * 如果只有 true/false，运维看到 "false" 得回去翻配置才知道是哪种。
     */
    if (!s_enabled) {
        return "disabled";
    }
    return mqtt_mgr_is_connected() ? "connected" : "disconnected";
}

esp_err_t mqtt_mgr_publish(const char *suffix, const char *payload,
                           int len, int qos, bool retain)
{
    if (suffix == NULL || suffix[0] == '\0' || payload == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_enabled || s_client == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * ★ 未连接时直接失败，**不调 esp_mqtt_client_publish()**。
     *
     * 这是本文件最重要的一个判断。esp-mqtt 在未连接时会"愉快地"
     * 接受这条消息（QoS1/2 进 outbox，返回正常的 msg_id），
     * 然后 30 秒后静默丢弃。调用方如果按返回值判断，会以为发送成功，
     * 于是自己的"已发送计数"和 broker 上的实际消息数永远对不上，
     * 而且找不到任何线索。
     *
     * 在这里挡住，调用方拿到明确的 ESP_ERR_INVALID_STATE，
     * 就能自己决定是丢弃还是缓存——本工程的 mqtt_task 选择丢弃并计数。
     */
    if (!mqtt_mgr_is_connected()) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 前缀 + 后缀。栈上局部缓冲即可：publish 会把 topic 拷进报文 */
    char topic[MQTT_MGR_TOPIC_MAX];
    const int n = snprintf(topic, sizeof(topic), "%s%s", s_topic_prefix, suffix);
    if (n < 0 || n >= (int)sizeof(topic)) {
        ESP_LOGE(TAG, "topic 太长（前缀 %s + 后缀 %s）", s_topic_prefix, suffix);
        return ESP_ERR_INVALID_ARG;
    }

    const int msg_id = esp_mqtt_client_publish(s_client, topic, payload, len,
                                               qos, retain ? 1 : 0);
    if (msg_id == -2) {
        /* outbox 满。正常路径下不该发生（未连接时已被上面挡住），
         * 但如果 broker 收得比我们发得慢，仍可能积压 */
        ESP_LOGW(TAG, "outbox 已满，本条丢弃：%s", topic);
        return ESP_ERR_NO_MEM;
    }
    if (msg_id < 0) {
        ESP_LOGW(TAG, "发布失败（msg_id=%d）：%s", msg_id, topic);
        return ESP_FAIL;
    }

    return ESP_OK;
}
