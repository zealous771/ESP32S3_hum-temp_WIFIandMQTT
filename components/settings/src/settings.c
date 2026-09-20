/*
 * settings.c —— 基于 NVS 的配置存取
 *
 * 实现很薄，但有两个细节值得单独说。
 */
#include "settings.h"

#include <stdbool.h>

#include "esp_log.h"

#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "settings";

/*
 * 命名空间与键名。
 *
 * 命名空间（namespace）用来把不同用途的键分开，避免撞名——
 * 相当于 NVS 里的一层目录。同一个命名空间下的键名必须唯一。
 *
 * 键名长度上限是 15 个字符（NVS_KEY_NAME_MAX_SIZE - 1），
 * 超了 nvs_set_xxx 会返回 ESP_ERR_NVS_KEY_TOO_LONG。
 * 这个限制来自 NVS 内部的存储格式，不是随意定的。
 */
#define NVS_NAMESPACE   "app_cfg"
#define KEY_PERIOD_MS   "period_ms"

/* NVS 句柄。打开一次，之后所有读写复用它 */
static nvs_handle_t s_handle;
static bool s_ready = false;

esp_err_t settings_init(void)
{
    esp_err_t ret = nvs_flash_init();

    /*
     * 有两种"看起来是错误、实际需要擦除重来"的情况：
     *
     *   ESP_ERR_NVS_NO_FREE_PAGES
     *       分区里没有空闲页了。可能是数据写满，也可能是残留数据格式不对。
     *
     *   ESP_ERR_NVS_NEW_VERSION_FOUND
     *       NVS 内部格式版本比当前 IDF 期望的新（通常是 IDF 升级过）。
     *
     * 这两种情况都必须擦掉整个 nvs 分区才能继续用。代价是丢配置，
     * 但不擦的话 NVS 完全不可用。生产环境里可以考虑先尝试读出能读的键
     * 再擦，本组件为简洁起见直接擦。
     */
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "nvs 分区需要重建（%s），擦除后重试", esp_err_to_name(ret));
        ret = nvs_flash_erase();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "擦除 nvs 分区失败: %s", esp_err_to_name(ret));
            return ret;
        }
        ret = nvs_flash_init();
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "初始化 nvs 失败: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 打开命名空间。NVS_READWRITE 表示可读可写；不存在会自动创建 */
    ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &s_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "打开命名空间 \"%s\" 失败: %s", NVS_NAMESPACE, esp_err_to_name(ret));
        return ret;
    }

    s_ready = true;
    ESP_LOGI(TAG, "NVS 就绪，命名空间 \"%s\"", NVS_NAMESPACE);
    return ESP_OK;
}

uint32_t settings_get_period_ms(uint32_t fallback)
{
    if (!s_ready) {
        return fallback;
    }

    uint32_t value = 0;
    esp_err_t ret = nvs_get_u32(s_handle, KEY_PERIOD_MS, &value);

    /*
     * "键不存在"不是错误，是**第一次开机**的正常情况。
     * 所以这里单独判断 ESP_ERR_NVS_NOT_FOUND，安静地返回默认值。
     *
     * 这个设计让调用方不需要区分"第一次开机"和"保存过"两种情况——
     * 传一个默认值进来，两种情况走同一段代码。
     */
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "尚未保存过周期，使用默认值 %u ms", (unsigned)fallback);
        return fallback;
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "读取周期失败（%s），使用默认值", esp_err_to_name(ret));
        return fallback;
    }

    ESP_LOGI(TAG, "读回上次保存的周期: %u ms", (unsigned)value);
    return value;
}

esp_err_t settings_set_period_ms(uint32_t period_ms)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * ★ 关键：nvs_set_xxx 只写进**内存缓存**，不会立刻落盘。
     *   必须再调 nvs_commit() 才会真正写入 Flash。
     *
     *   分开两步是有意的——如果你要改十个键，可以十次 set 之后
     *   只 commit 一次，避免十次 Flash 写入。
     *   本组件只有一个键，所以 set 完立刻 commit。
     *
     *   忘记 commit 的症状很有迷惑性：程序跑起来读得到值（因为
     *   内存缓存里是新的），一断电就没了。
     */
    esp_err_t ret = nvs_set_u32(s_handle, KEY_PERIOD_MS, period_ms);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "写入周期失败: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = nvs_commit(s_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "提交失败（数据未落盘）: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "周期已保存到 NVS: %u ms", (unsigned)period_ms);
    return ESP_OK;
}
