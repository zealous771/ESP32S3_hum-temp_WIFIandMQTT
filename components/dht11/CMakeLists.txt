# dht11 组件的注册声明
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
#                   本组件没有私有头文件，所以留空字符串。
#   REQUIRES        公开依赖：会传递给"依赖你的组件"。
#                   判据是——你的**头文件**里 include 了对方的头文件。
#   PRIV_REQUIRES   私有依赖：不传递。
#                   判据是——只有你的**源文件**里用到了对方。
#
#
# 依赖逐条判定（每条都给出结论和依据）：
#
#   PRIV_REQUIRES esp_driver_gpio  ← 要写
#       dht11.h 里没有 include 任何 GPIO 的头文件（它只用到 esp_err.h
#       和 stdint.h），是 dht11.c 内部才去调 gpio_config()，以及直接读
#       寄存器用的 gpio_ll_get_level() / gpio_ll_set_level()。
#       按上面的判据，这属于"只在源文件里用"，所以是 PRIV_REQUIRES。
#       写成 REQUIRES 的坏处：所有依赖 dht11 的组件都会顺带"看得见"
#       driver/gpio.h，白白扩大耦合面，也让依赖图失真。
#
#   PRIV_REQUIRES esp_timer        ← 要写！这一步很容易漏
#       dht11.c 用 esp_timer_get_time() 记录"上次成功读取的时刻"，
#       用来实现最小读取间隔保护。esp_timer.h 由 esp_timer 组件提供。
#
#       关键点：esp_timer **不是**公共组件（common component）。
#       ESP-IDF 的公共依赖列表定义在 tools/cmake/build.cmake 里，内容是
#           cxx esp_libc freertos esp_hw_support heap log soc hal
#           esp_rom esp_common esp_system esp_stdio
#       ——里面没有 esp_timer。esp_timer 只是 esp_hw_support 和 esp_system
#       的 PRIV_REQUIRES，而私有依赖**不会传递**。
#
#       这一点是可以实测验证的，不是推理：本工程 build/compile_commands.json
#       里，components/led/src/led.c 和 components/button/src/button.c 的编译
#       命令行中都**找不到** esp_timer/include 这条 -I。
#       而这两个组件的 CMakeLists 与本文件的结构完全一样。
#       所以：如果这里不写 esp_timer，dht11.c 会以
#       "esp_timer.h: No such file or directory" 编译失败。
#
#   esp_hal_gpio  ← 故意**不**写
#       既然 dht11.c 要 include "hal/gpio_ll.h"，直觉上应该把 esp_hal_gpio
#       也写进来。但它不需要：esp_driver_gpio 在自己的 CMakeLists 里用
#       REQUIRES（公开依赖）声明了 esp_hal_gpio，而公开依赖是会**传递**的——
#       依赖 esp_driver_gpio 的组件自动就能看到 esp_hal_gpio 的头文件目录。
#
#       这一点同样是从本工程的 build/compile_commands.json 里验证过的：
#       gpio 相关使用方的编译命令里已经带着
#           .../esp_hal_gpio/include
#           .../esp_hal_gpio/esp32s3/include
#       两条 -I。多写一条不会报错，但会让"谁真正依赖谁"变得看不清。
#
#   esp_hw_support  ← 不写，因为它是公共组件
#       dht11.c 用 esp_cpu_get_cycle_count() 读 CCOUNT 寄存器，
#       声明在 esp_cpu.h，由 esp_hw_support 提供。
#       esp_hw_support 在上面那份公共依赖列表里，构建系统会把它自动加到
#       **每一个**组件的依赖里，所以 esp_cpu.h 天然可见。
#       （顺带一提：也正因为它进了公共列表，include 它的成本是零，
#        不需要也不应该为了"省依赖"去绕开它。）
#
#   freertos、log  ← 不写，同样是公共组件
#       freertos/FreeRTOS.h（portSET_INTERRUPT_MASK_FROM_ISR、
#       portMUX_TYPE 等）和 esp_log.h 都由公共组件提供，自动可见。
#       工程里 led / button 组件也是这么处理的，属于既定惯例。
#
idf_component_register(SRCS "src/dht11.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS ""
                       PRIV_REQUIRES esp_driver_gpio esp_timer)
