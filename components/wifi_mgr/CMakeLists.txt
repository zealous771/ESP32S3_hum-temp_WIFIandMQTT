# wifi_mgr 组件的注册声明
#
# 逐个关键字说明，以及"为什么是 PRIV_REQUIRES 而不是 REQUIRES"：
#
#   SRCS "src/wifi_mgr.c"
#     本组件的唯一源文件。路径相对组件根目录（components/wifi_mgr/）。
#
#   INCLUDE_DIRS "include"
#     公开头文件目录。wifi_mgr.h 放在这里，别的组件写
#     #include "wifi_mgr.h" 就能找到——上层（app 组件）调用本组件靠这行。
#
#   PRIV_INCLUDE_DIRS ""
#     本组件没有只给自己用的私有头文件，所以留空。
#     （对比 app 组件：它把 app_internal.h 放在 src/ 并在这里声明 "src"。）
#
#   PRIV_REQUIRES esp_wifi esp_netif esp_event
#     私有依赖，理由见下面的三条 verdict。
#
# ── verdict 1：esp_wifi / esp_netif / esp_event 是**私有**依赖 ──────
#
#   它们只在 wifi_mgr.c 里出现，wifi_mgr.h 一行都没用到——使用者只需要
#   知道"传 SSID 和密码、查连没连上"，不需要看见驱动类型。
#   所以写 PRIV_REQUIRES：不会被传递给依赖本组件的代码，ESP-IDF 的细节
#   被挡在组件内部。写成 REQUIRES 会让上层也"看得见"这些头文件，
#   那是多余的耦合。
#
#   为什么必须显式声明？
#     ESP-IDF 只把一小撮组件当公共组件自动加进每个组件的依赖里
#     （freertos、log、esp_common 之类；本项目核实过这 13 个里
#      **不含** esp_wifi / esp_netif / esp_event），
#     其余组件必须自己写进 REQUIRES / PRIV_REQUIRES，
#     否则编译时直接报"找不到头文件"。
#
#   为什么 esp_netif 也要列？
#     wifi_mgr.c 调用的 esp_netif_init() 实现在 esp_netif 组件里；
#     esp_netif_t 类型、ip_event_got_ip_t、打印 IP 用的 IPSTR / IP2STR
#     也都由 esp_netif 提供。
#
#     只声明 esp_wifi 其实也能编过——因为 esp_wifi 的 CMakeLists 里写着
#     REQUIRES esp_event esp_phy esp_netif，这条公开依赖会把 esp_netif 的
#     头文件传递过来。但那是"借来的"可见性：哪天 esp_wifi 把它改成私有，
#     本组件就会在别人的改动里突然编译失败。**自己用到的组件自己声明**，
#     这才是 IDF 的惯例。
#
#     顺便记一个容易搞错的点（对照 v6.1 源码核实过）：
#       esp_netif_create_default_wifi_sta() 声明在
#       esp_wifi/include/esp_wifi_default.h，而它的**实现也在 esp_wifi**
#       （esp_wifi/src/wifi_default.c），并不在 esp_netif 组件里。
#       函数名带 esp_netif 前缀，很容易让人以为实现属于 esp_netif——
#       不是。这正是"头文件和实现可以不在同一个组件里"的典型例子，
#       也说明声明依赖时不能只看函数名。
#
# ── verdict 2：nvs_flash **故意不列**（既不声明也不调用） ───────────
#
#   本组件一行 nvs_flash_init() 都没有，因为职责已经有人担了：
#   settings 组件会调 nvs_flash_init()，并且在遇到
#   ESP_ERR_NVS_NO_FREE_PAGES / ESP_ERR_NVS_NEW_VERSION_FOUND 时
#   先 nvs_flash_erase() 再重试；而 settings_init() 在 app_start() 的
#   **最开始**执行，早于所有外设初始化。
#
#   所以 wifi_mgr_init() 运行时 NVS 早已挂载好：
#     - 这里再调一次是重复职责；两个组件各自擦一次分区还会互相竞争
#       （一方正在擦除时另一方在读写，行为不可预期）。
#     - 本工程 CONFIG_ESP_WIFI_NVS_ENABLED=y，WiFi 驱动自己需要用 NVS
#       保存射频校准数据，而它用的就是那个已经挂载好的 nvs 分区，
#       不需要本组件为它做任何事。
#
# ── verdict 3：lwip 不列；freertos / log 是公共组件，永远不写 ──────
#
#   lwip：wifi_mgr.c 不用 lwIP 的任何接口。打印 IP 用的 IPSTR / IP2STR
#     宏定义在 esp_netif 的公开头文件 esp_netif_ip_addr.h 里，
#     跟着 esp_netif 一起可见，所以不需要 lwip。
#     （esp_wifi 自己确实依赖 lwip，但那是它的 PRIV_REQUIRES，
#      不会传递给使用方——顺带一提，就算想靠它"蹭"到 lwip 头文件也蹭不到。）
#
#   freertos：xEventGroupCreate / xEventGroupWaitBits / pdMS_TO_TICKS
#     来自 freertos，而 freertos 是公共组件，构建系统自动加，不写。
#   log：ESP_LOGI/W/E 来自 log，同样是公共组件，不写。
#
idf_component_register(SRCS "src/wifi_mgr.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS ""
                       PRIV_REQUIRES esp_wifi esp_netif esp_event)
