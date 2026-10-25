file(GLOB __src ${CMAKE_CURRENT_LIST_DIR}/*.cc ${CMAKE_CURRENT_LIST_DIR}/*.c)

# 无线域（main/wireless/）：帧编解码（frame）、ESP-NOW 链路（link）、ARQ 核心
# 与适配层（arq / arq_link）、配对（pairing）。两端共用，无条件编译。
list(APPEND __src
    ${CMAKE_CURRENT_LIST_DIR}/wireless/frame.cc
    ${CMAKE_CURRENT_LIST_DIR}/wireless/espnow_transport.cc
    ${CMAKE_CURRENT_LIST_DIR}/wireless/link.cc
    ${CMAKE_CURRENT_LIST_DIR}/wireless/arq.cc
    ${CMAKE_CURRENT_LIST_DIR}/wireless/arq_link.cc
    ${CMAKE_CURRENT_LIST_DIR}/wireless/pairing.cc)

# 本机串口桥（UART <-> 无线；Local 模式下 UART <-> 主机 CDC）：两端共用一份实现，
# 对侧端口的差异在文件内用 CONFIG_MEGUMINLINK_USB_SUPPORT 表达，无条件编译。
list(APPEND __src ${CMAKE_CURRENT_LIST_DIR}/uart/uart_bridge.cc)

# main/usb/ 子目录（TinyUSB）仅在 CONFIG_MEGUMINLINK_USB_SUPPORT 开启时编译，
# 该选项仅对 ESP32-S3 可用（见同目录 Kconfig.projbuild）。
# 源文件逐个登记：glob 的结果在配置期固定，新增文件不会自动进构建。
if(CONFIG_MEGUMINLINK_USB_SUPPORT)
    list(APPEND __src
        ${CMAKE_CURRENT_LIST_DIR}/usb/usb.cc
        ${CMAKE_CURRENT_LIST_DIR}/usb/usb_descriptors.cc
        ${CMAKE_CURRENT_LIST_DIR}/usb/cdc_acm.cc
        ${CMAKE_CURRENT_LIST_DIR}/usb/usb_capability.cc
        ${CMAKE_CURRENT_LIST_DIR}/usb/update_mode.cc
        ${CMAKE_CURRENT_LIST_DIR}/usb/firmware_updater.cc)
endif()

# CMSIS-DAP 固件（移植自 Arm CMSIS-DAP V2.1.2）：无条件编译。
# Local 模式（USB 本地执行）与无线接收端（target 侧执行 DAP 命令）
# 都依赖 DAP 核心；无线接收端通常是无 USB 的目标（如 ESP32-C3）。
file(GLOB __dap_src ${CMAKE_CURRENT_LIST_DIR}/cmsis_dap/Source/*.c)
list(APPEND __src ${__dap_src})

# CMSIS-Driver USART 适配层（Driver_USART0，SWO UART 捕获的接收通路）：
# SWO_UART=0 的构建（非 USB 目标）里文件内容为空，保持登记以维持清单完整。
list(APPEND __src ${CMAKE_CURRENT_LIST_DIR}/cmsis_dap/Config/Driver_USART.cc)

# LED 指示灯模块：两个驱动的源文件都无条件编译——公共接口（led_types.hpp 的基类与
# concept）改动时，未被选中的那个驱动也必须在编译期暴露出来，否则它会在没有板子使用
# 的情况下悄悄腐烂。未被选中的驱动不会被引用，链接器不会把它取进镜像；Disabled 时
# 选型回退到空驱动（见 NullLedDriver），调用方也不必判断 LED 是否编进固件。
list(APPEND __src
    ${CMAKE_CURRENT_LIST_DIR}/led/gpio_led_driver.cc
    ${CMAKE_CURRENT_LIST_DIR}/led/ws2812_led_driver.cc)

file(GLOB __meaproto_src ${CMAKE_CURRENT_LIST_DIR}/meaproto/*.cc)
list(APPEND __src ${__meaproto_src})

idf_component_register(SRCS ${__src}
                    INCLUDE_DIRS "./include" "./cmsis_dap/Include" "./cmsis_dap/Config")
