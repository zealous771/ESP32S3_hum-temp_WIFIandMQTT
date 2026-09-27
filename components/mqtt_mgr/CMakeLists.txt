# mqtt_mgr 组件的注册声明
#
# 这个组件管理 MQTT 连接：连、断线重连、状态查询、publish。
# 形状与 wifi_mgr 保持一致（不阻塞 init + 可选 wait + 随时查）。
#
# 依赖说明：
#
#   espressif/mqtt（托管组件）
#     由同目录的 idf_component.yml 声明，**不写在这里**。
#     组件管理器会把它注入到本组件的依赖里，头文件 mqtt_client.h 因此可见。
#
#     为什么不在 CMakeLists 里写 REQUIRES mqtt：
#     因为 esp-mqtt 在 IDF v6.1 里已经不是一个内置组件名了
#     （它被移出核心，见 idf_component.yml 里的证据），
#     在 CMakeLists 里硬写一个不存在的组件名会直接配置失败。
#
#   PRIV_REQUIRES esp_event
#     mqtt_mgr.c 的事件回调签名用到 esp_event_base_t。
#
#     严格说 esp_event 可以省掉：mqtt 组件是 `REQUIRES esp_event tcp_transport`
#     （公开依赖，会传递下来），所以不声明也能编过。
#     但**自己用到的东西自己声明**更诚实——哪天 mqtt 把 esp_event 降级成
#     PRIV_REQUIRES，这里就会突然编译失败，而错误信息完全指不到原因。
#     显式声明换来的是"改上游不会莫名其妙炸掉"。
#
#   写成 PRIV_REQUIRES 而不是 REQUIRES：
#     mqtt_mgr.h 里没有任何来自 esp_event 或 mqtt 的类型
#     （只用到了 stdbool/stdint/esp_err），调用方只需要 include "mqtt_mgr.h"，
#     不该顺带"看得见"底层的 mqtt_client.h。
#     这正是本工程"依赖按需声明、不制造多余可见性"的一贯做法。
#
#   freertos / log 是公共组件，会自动加到每个组件，无需声明。
idf_component_register(SRCS "src/mqtt_mgr.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS ""
                       PRIV_REQUIRES esp_event)
