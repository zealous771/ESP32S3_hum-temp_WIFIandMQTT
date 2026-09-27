# time_mgr 组件的注册声明
#
# 这个组件把 SNTP（网络时间同步）封装成"不阻塞 init + 可选 wait +
# 随时可查"三件事，形状与 wifi_mgr 一致。
#
# 依赖说明：
#
#   PRIV_REQUIRES esp_netif lwip esp_timer
#
#     esp_netif —— esp_netif_sntp.h 里的 esp_netif_sntp_init() 等接口。
#                  它是 ESP-IDF 在 lwIP 裸 SNTP 外面包的一层，
#                  负责与默认网络接口和事件循环衔接。
#
#     lwip      —— 真正干活的 sntp_* 实现在这里。**必须显式声明**：
#                  esp_netif 对 lwip 的依赖不会传递过来（这条规则在本工程
#                  已经踩过好几次），只 include 头文件不改依赖的话，
#                  编译能过但链接会报 undefined reference。
#
#     esp_timer —— time_mgr_boot_ms_to_unix_ms() 要读 esp_timer_get_time()
#                  来算"开机时钟与真实时间的偏差"。esp_timer **不是**
#                  公共组件（见 dht11/CMakeLists.txt 里的同类说明），
#                  不声明同样会链接失败。
#
#   写成 PRIV_REQUIRES 而不是 REQUIRES：time_mgr.h 里没有任何来自这三个
#   组件的类型（只用到了 stdbool/stdint/esp_err），调用方只需要 include
#   "time_mgr.h"，不该顺带"看得见" esp_netif 和 lwip 的头文件。
#   这正是本工程"依赖按需声明、不制造多余可见性"的一贯做法。
#
#   freertos / log 是公共组件，会自动加到每个组件，无需声明。
idf_component_register(SRCS "src/time_mgr.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS ""
                       PRIV_REQUIRES esp_netif lwip esp_timer)
