# led 组件的注册声明
#
# 这个文件回答一个问题：构建系统怎么知道有这个组件？
# 答案是——它不需要你登记。构建系统会自动扫描 IDF_PATH/components
# 和工程根目录下的 components/，发现子目录里有 CMakeLists.txt 且调用了
# idf_component_register()，就把它当作一个组件纳入构建。
#
# 各参数的含义：
#
#   SRCS            要编译的源文件。用相对本目录的路径。
#   INCLUDE_DIRS    对外公开的头文件目录。写了这里，别的组件才能 #include 到。
#   PRIV_INCLUDE_DIRS  只给本组件内部用的头文件目录，不对外暴露。
#
#   REQUIRES        公开依赖：会传递给"依赖你的组件"。
#                   判据是——你的**头文件**里 include 了对方的头文件。
#   PRIV_REQUIRES   私有依赖：不传递。
#                   判据是——只有你的**源文件**里用到了对方。
#
# 这里用 PRIV_REQUIRES 而不是 REQUIRES，因为 led.h 里没有 include 任何
# GPIO 的头文件（只用到了 esp_err.h 和 stdbool.h），
# 是 led.c 内部才去调 gpio_set_level() 等接口。
# 如果写成 REQUIRES，会让依赖本组件的代码也"看得见"GPIO 的头文件，
# 那属于不必要的耦合。
idf_component_register(SRCS "src/led.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS ""
                       PRIV_REQUIRES esp_driver_gpio)
