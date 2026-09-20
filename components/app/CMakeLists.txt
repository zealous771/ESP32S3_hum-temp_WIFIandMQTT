# app 组件的注册声明
#
# 这个组件是"应用层"：把入口（main.c）和业务逻辑分开，
# 让 main.c 只负责声明板级配置。
#
# 依赖说明：
#
#   PRIV_REQUIRES led button rgb_led settings dht11 wifi_mgr
#     app.c 调用了 led_init() / button_init() / rgb_led_init() /
#     dht11_init() / wifi_mgr_init() / settings_init()，
#     各任务文件分别调用了对应组件的接口。
#     因为 app.h（公开头文件）里没有 include 这些组件的头文件，
#     属于"只在源文件里用"，所以是私有依赖。
#     写成 REQUIRES 会让依赖 app 的代码也"看得见"它们的头文件，
#     那是多余的耦合。
#
#     注意 wifi_mgr 内部还依赖 esp_wifi / esp_netif / esp_event，
#     但那三个是本组件看不见的——刚才那条"依赖不传递"的规则在这里
#     正好起了作用：app 只关心"连上了没"，不关心 WiFi 栈长什么样。
#
#   PRIV_INCLUDE_DIRS "src"
#     让 src/app_internal.h 能被本组件的各个 .c 找到，
#     同时不对外暴露——外部只应该 include "app.h"。
#
#   freertos / log 是公共组件，会自动加到每个组件，无需声明。
#
# 加新任务时，把新的 *_task.c 追加到 SRCS 里即可。
idf_component_register(SRCS "src/app.c"
                            "src/blink_task.c"
                            "src/button_task.c"
                            "src/rgb_task.c"
                            "src/telemetry_task.c"
                            "src/telemetry_json.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "src"
                       PRIV_REQUIRES led button rgb_led settings dht11 wifi_mgr)
