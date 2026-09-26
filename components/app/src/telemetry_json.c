/*
 * telemetry_json.c —— 把一次读数序列化成一行 JSON
 *
 * 三个设计取舍，都是有意的：
 *
 * 1. 自己拼字符串，不用 cJSON。
 *    报文只有六个字段、结构固定，用 snprintf 一行就能写完。
 *    引入 JSON 库要多占几 KB Flash，还要在堆上分配/释放对象——
 *    在内存紧张的 MCU 上，为了六个字段付这个代价不划算，
 *    而且多一层堆分配就多一种"分配失败"的失败模式。
 *    反过来，如果哪天报文变成嵌套的、字段数量不定的，那才值得上库。
 *
 * 2. 一次 snprintf 写完，不做分段拼接。
 *    分段 strcat 需要自己盯着剩余空间，很容易写出越界；
 *    snprintf 把"边界检查"和"写入"合并成一次调用，少一处出错的地方。
 *    代价是必须**读懂它的返回值**——见下面截断判断处的注释。
 *
 * 3. 失败就返回 -1，绝不返回"半个 JSON"。
 *    被截断的报文发出去，接收端只会报解析错误且看不出原因；
 *    与其让错误在链路的另一头暴露，不如在生成端就拒绝。
 */
#include "telemetry_json.h"

#include <stdio.h>

#include "esp_log.h"

static const char *TAG = "telemetry_json";

int app_format_reading_json(char *buf, size_t len, const sensor_data_t *d, bool wifi_ok)
{
    /*
     * 参数检查放最前面。
     *
     * 这些都属于"调用方的编程错误"，不是运行期故障，但仍然记一条
     * ESP_LOGE：直接返回 -1 会让人以为"只是这次没数据"，排错时找不到线索。
     * （注意：组件内部一律返回错误码，不用 assert/ESP_ERROR_CHECK——
     *   断言在 release 配置下会被编译掉，剩下的越界访问才是真麻烦。）
     */
    if (buf == NULL || d == NULL || len == 0) {
        ESP_LOGE(TAG, "参数非法（buf=%p, d=%p, len=%u），报文未生成",
                 (const void *)buf, (const void *)d, (unsigned)len);
        return -1;
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
     */
    const int written = snprintf(buf, len,
                                 "{\"device_id\":\"%s\",\"ts\":%lld,\"seq\":%u,"
                                 "\"temperature_c\":%d,\"humidity_rh\":%d,\"wifi\":%s}",
                                 d->device_id, (long long)d->timestamp_ms,
                                 (unsigned)d->seq, d->temperature, d->humidity,
                                 wifi_ok ? "true" : "false");

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
