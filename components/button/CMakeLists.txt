# button 组件的注册声明
#
# 依赖说明：
#   PRIV_REQUIRES esp_driver_gpio —— button.c 里用到了 gpio_config / gpio_get_level /
#                                    gpio_install_isr_service 等 GPIO 驱动接口。
#                                    因为 button.h 里没有 include GPIO 的头文件，
#                                    属于"只在源文件里用"，所以是私有依赖。
#
#   FreeRTOS 的队列接口（xQueueCreate / xQueueSendFromISR）不需要声明，
#   因为 freertos 是公共组件，会自动加到每个组件的依赖里。
idf_component_register(SRCS "src/button.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS ""
                       PRIV_REQUIRES esp_driver_gpio)
