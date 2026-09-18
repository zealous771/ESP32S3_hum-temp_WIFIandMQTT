# main 是一个特殊组件：它总是被编译，是程序入口。
# 它同样需要用 idf_component_register() 声明，也同样要写清依赖。
#
# PRIV_REQUIRES led —— 因为 main.c 里 #include 了 "led.h"。
# 严格来说头文件引用应该用 REQUIRES，但 led.h 只在 main.c 内部使用，
# 不会传递给别人，所以用 PRIV_REQUIRES 更准确地表达"这是内部依赖"。
#
# 不需要声明 freertos / log（esp_log.h 所属组件）这些：
# 它们是"公共组件"，构建系统会自动加到每个组件的依赖里。
# 当前工程实际生效的公共组件清单见 build/project_description.json 的
# common_component_reqs 字段，共 13 个。
idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES led)
