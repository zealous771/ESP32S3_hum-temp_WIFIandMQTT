# rgb_led 组件的注册声明
#
# 依赖说明：
#
#   led_strip 为什么没有写在 PRIV_REQUIRES 里？
#
#     因为它是通过同目录下的 idf_component.yml 声明的**组件管理器依赖**。
#     构建系统在解析组件依赖时会调用组件管理器的 inject_requirements，
#     把 manifest 里声明的依赖自动补进本组件的 REQUIRES/PRIV_REQUIRES。
#     所以这里不需要（也不应该）重复写一遍——写了反而会在组件还没被
#     下载下来时找不到而报错。
#
#     这是 ESP-IDF 里两种依赖来源的分工：
#       CMakeLists.txt   —— 依赖工程内或 IDF 自带的组件
#       idf_component.yml —— 依赖需要从组件仓库下载的组件
#
#   esp_driver_rmt 也不需要声明：
#     rgb_led.c 里没有 include 任何 RMT 的头文件，只用 led_strip 的接口。
#     RMT 是 led_strip 内部的事，由它自己去依赖。
#
#   freertos / log 是公共组件，自动加，无需声明。
idf_component_register(SRCS "src/rgb_led.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "")
