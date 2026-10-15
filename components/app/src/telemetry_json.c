/*
 * telemetry_json.c —— 把一次采样记录组装成一行上报报文
 *
 * 三个设计取舍，都是有意的：
 *
 * 1. 自己拼字符串，不用 cJSON。
 *    报文只有八个字段、结构固定，用 snprintf 一行就能写完。
 *    引入 JSON 库要多占几 KB Flash，还要在堆上分配/释放对象——
 *    在内存紧张的 MCU 上，为了八个字段付这个代价不划算，
 *    而且多一层堆分配就多一种"分配失败"的失败模式。
 *    反过来，如果哪天报文变成嵌套的、字段数量不定的，那才值得上库。
 *
 * 2. 一次 snprintf 写完，不做分段拼接。
 *    分段 strcat 需要自己盯着剩余空间，很容易写出越界；
 *    snprintf 把"边界检查"和"写入"合并成一次调用，少一处出错的地方。
 *    代价是必须**读懂它的返回值**——见下面截断判断处的注释。
 *    （唯一的例外是 rssi 的 null：它要在拼主串之前先决定好，
 *      见下面的说明。那也是先拼成一个小串再 %s 进去，没有分段拼接。）
 *
 * 3. 失败就返回 -1，绝不返回"半个 JSON"。
 *    被截断的报文发出去，接收端只会报解析错误且看不出原因；
 *    与其让错误在链路的另一头暴露，不如在生成端就拒绝。
 */
#include "telemetry_json.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "time_mgr.h"
#include "wifi_mgr.h"

static const char *TAG = "telemetry_json";

void app_collect_meta(telemetry_meta_t *meta, int64_t boot_ms)
{
    if (meta == NULL) {
        return;
    }

    /*
     * WiFi 连接状态。任意任务可调，未初始化时返回 false，不阻塞。
     */
    meta->wifi_ok = wifi_mgr_is_connected();

    /*
     * RSSI。注意**先查连接状态、失败就标 invalid**，
     * 而不是"读失败就当 0"。0 dBm 是合法且极强的读数，
     * 用它表示"读不到"会把真实数据污染掉。
     *
     * 这里也不再单独查一次 wifi_mgr_is_connected()：
     * wifi_mgr_get_rssi() 内部已经判过，失败就返回
     * ESP_ERR_WIFI_NOT_CONNECT，语义正好对应 rssi_valid = false。
     * 多查一次只会多一个可能与上面 wifi_ok 不一致的来源。
     */
    int8_t rssi = 0;
    if (wifi_mgr_get_rssi(&rssi) == ESP_OK) {
        meta->rssi_valid = true;
        meta->rssi       = rssi;
    } else {
        meta->rssi_valid = false;
        meta->rssi       = 0;   /* 占位，rssi_valid 为假时不会被用到 */
    }

    /*
     * 时间戳换算。
     *
     * ★ 这里是整个时间设计的关键一步：队列里存的一直是**开机毫秒**，
     *   直到这一刻（报文即将离开设备）才换算成 Unix 毫秒。
     *   换算用 "boot_ms + (当前墙钟 − 当前开机时钟)"，括号里是常量偏差，
     *   所以"同步前采样、同步后发出"的数据也能拿到**正确的** Unix 时间。
     *
     *   如果反过来在采样时就写 Unix 时间，同步前采的数据就永久失去了
     *   正确时间信息，事后无法补救。
     *
     * 未同步时 boot_ms_to_unix_ms() 原样返回 boot_ms，此时
     * ts_is_unix = false，报文里 clock 字段会标成 "boot"。
     * 两个信息都来自 time_mgr，不会互相矛盾。
     */
    meta->ts_is_unix = time_mgr_is_synced();
    meta->ts_ms      = time_mgr_boot_ms_to_unix_ms(boot_ms);
}

int app_format_reading_json(char *buf, size_t len, const sensor_data_t *d,
                            const telemetry_meta_t *meta)
{
    /*
     * 参数检查放最前面。
     *
     * 这些都属于"调用方的编程错误"，不是运行期故障，但仍然记一条
     * ESP_LOGE：直接返回 -1 会让人以为"只是这次没数据"，排错时找不到线索。
     * （注意：组件内部一律返回错误码，不用 assert/ESP_ERROR_CHECK——
     *   断言在 release 配置下会被编译掉，剩下的越界访问才是真麻烦。）
     */
    if (buf == NULL || d == NULL || meta == NULL || len == 0) {
        ESP_LOGE(TAG, "参数非法（buf=%p, d=%p, meta=%p, len=%u），报文未生成",
                 (const void *)buf, (const void *)d, (const void *)meta,
                 (unsigned)len);
        return -1;
    }

    /*
     * rssi 字段：有效时是数字，无效时是 **null**。
     *
     * 为什么用 null 而不是省略这个字段？
     *   省略会让报文的 schema 不稳定——接收端一会儿有这个键、
     *   一会儿没有，解析代码就得写"有没有"的分支。
     *   恒发这个键、值可以是 null，schema 就固定了，
     *   而 null 在 JSON 里本来就表达"这个值未知"。
     *
     * 为什么要先拼成一个字符串再 %s 进去，而不是在主格式串里用
     * 三元运算符选 "%d" 或 "null"？
     *   因为那样格式串本身就成了变量，而"格式串是数据"是
     *   printf 家族最经典的坑（见 console_task.c 里的同类说明）。
     *   先把值变成一个确定的字符串，主格式串保持字面量，
     *   就没有这个风险。
     *
     * 缓冲 8 字节够放 "-127" 或 "null" 加结尾。
     */
    char rssi_str[8];
    if (meta->rssi_valid) {
        snprintf(rssi_str, sizeof(rssi_str), "%d", (int)meta->rssi);
    } else {
        strcpy(rssi_str, "null");
    }

    /*
     * device_id 直接进格式串的 %s。这里**不做** JSON 转义，
     * 因为 device_id 是本机配置的固定标识，不含引号或反斜杠；
     * 一旦将来它可能来自外部输入（配网页面、NVS 里的用户输入），
     * 就必须补上转义，否则一个引号就能把整条报文的结构破坏掉。
     *
     * ts 用 %lld：int64_t 在 xtensa 上是 long long，%ld 会截断成 32 位。
     *
     * wifi 字段用 %s + 三元运算符，而不是 %d 打 0/1。
     * JSON 里 true/false 是布尔字面量，打 0/1 的话接收端拿到的是数字，
     * 类型和字段语义对不上（有些严格的解析器会直接判类型错误）。
     *
     * clock 恒发，取值 "unix" / "boot"，让接收端不必靠数值大小猜时间基准。
     */
    const int written = snprintf(buf, len,
                                 "{\"device_id\":\"%s\",\"ts\":%lld,\"seq\":%u,"
                                 "\"temperature_c\":%d,\"humidity_rh\":%d,"
                                 "\"wifi\":%s,\"rssi\":%s,\"clock\":\"%s\"}",
                                 d->device_id, (long long)meta->ts_ms,
                                 (unsigned)d->seq, d->temperature, d->humidity,
                                 meta->wifi_ok ? "true" : "false",
                                 rssi_str,
                                 meta->ts_is_unix ? "unix" : "boot");

    /*
     * ★ 关键：snprintf 返回的是"**本来会写多少字符**"（不含结尾 '\0'），
     *   不是"实际写了多少"。
     *
     *   所以截断的判据是 written >= (int)len，而不是 written == len：
     *   比如 len=16、需要 47 字节时，它返回 47，缓冲里只有 15 个字符
     *   加一个 '\0'。只比较"等于"会漏掉这个明显被截断的情况。
     *
     *   顺带一提：len 是 size_t（无符号），written 是 int。混着比较会
     *   把 written 隐式转成无符号，负数就变成天文数字。所以这里显式
     *   把 len 转成 int 再比。
     *
     *   written < 0 是理论上不该发生的编码错误，但既然它的类型允许为负，
     *   就必须判——这正是 C 里"返回 int 的长度函数"的经典坑。
     */
    if (written < 0 || written >= (int)len) {
        ESP_LOGE(TAG, "缓冲太小（需要 %d 字节，只有 %u 字节），报文未生成",
                 written, (unsigned)len);
        return -1;
    }

    return written;
}
