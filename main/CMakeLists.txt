# main 是一个特殊组件：它总是被编译，是程序入口。
# 它同样需要用 idf_component_register() 声明，也同样要写清依赖。
#
# PRIV_REQUIRES app —— main.c 里 #include 了 "app.h"。
#
# 注意这里不再需要 led / button：main.c 已经不再直接碰它们了，
# 那是 app 组件内部的事。这正是分层的价值——
# 入口文件只依赖应用层，不依赖具体外设。
#
# 不需要声明 freertos / log 这些：它们是"公共组件"，
# 构建系统会自动加到每个组件的依赖里。
# 当前工程实际生效的公共组件清单见 build/project_description.json 的
# common_component_reqs 字段，共 13 个。
idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES app)
