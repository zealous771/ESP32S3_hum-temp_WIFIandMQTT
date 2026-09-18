# main 是一个特殊组件：它总是被编译，是程序入口。
# 它同样需要用 idf_component_register() 声明，也同样要写清依赖。
#
# PRIV_REQUIRES led button —— main.c 里 #include 了 "led.h" 和 "button.h"。
# 这两个组件都在工程根目录的 components/ 下，构建系统会自动发现，无需登记。
#
# 不需要声明 freertos / log（esp_log.h 所属组件）这些：
# 它们是"公共组件"，构建系统会自动加到每个组件的依赖里。
# 当前工程实际生效的公共组件清单见 build/project_description.json 的
# common_component_reqs 字段，共 13 个。
idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES led button)
