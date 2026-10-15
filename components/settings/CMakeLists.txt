# settings 组件的注册声明
#
# 依赖说明：
#
#   PRIV_REQUIRES nvs_flash
#     settings.c 里用到了 nvs_flash_init / nvs_open / nvs_set_u32 等接口。
#     settings.h（公开头文件）里没有 include 任何 NVS 的头文件——
#     外部使用者只看到"读周期/写周期"，不需要知道底下是 NVS。
#     这正是封装的意义：以后想把存储换成 SD 卡或 FATFS，
#     只改本组件的实现，调用方一行都不用动。
#
#   注意 nvs_flash 是 IDF 自带组件，不需要写进 idf_component.yml
#   （那是给"需要从组件仓库下载"的组件用的，比如 rgb_led 依赖的 led_strip）。
#
#   freertos / log 是公共组件，自动加，无需声明。
idf_component_register(SRCS "src/settings.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS ""
                       PRIV_REQUIRES nvs_flash)
