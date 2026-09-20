file(GLOB __src ${CMAKE_CURRENT_LIST_DIR}/*.cc ${CMAKE_CURRENT_LIST_DIR}/*.c)

# 无线域（main/wireless/）：帧编解码（frame）、ESP-NOW 链路（link）、ARQ 核心
# 与适配层（arq / arq_link）、配对（pairing）。两端共用，无条件编译。
list(APPEND __src
    ${CMAKE_CURRENT_LIST_DIR}/wireless/frame.cc
    ${CMAKE_CURRENT_LIST_DIR}/wireless/link.cc
    ${CMAKE_CURRENT_LIST_DIR}/wireless/arq.cc
    ${CMAKE_CURRENT_LIST_DIR}/wireless/arq_link.cc
    ${CMAKE_CURRENT_LIST_DIR}/wireless/pairing.cc)

# 目标侧无线 UART 桥（UART <-> ESP-NOW）：仅无 USB 端（如 ESP32-C3 接收端）
# 编译。S3 侧串口走 USB CDC（usb/uart_bridge.cc 按模式分流），本文件不参与
# USB 构建。
if(NOT CONFIG_MEGUMINLINK_USB_SUPPORT)
    list(APPEND __src ${CMAKE_CURRENT_LIST_DIR}/wireless/uart.cc)
endif()

# main/usb/ 子目录（TinyUSB + UART 桥）仅在 CONFIG_MEGUMINLINK_USB_SUPPORT 开启时编译，
# 该选项仅对 ESP32-S3 可用（见同目录 Kconfig.projbuild）
if(CONFIG_MEGUMINLINK_USB_SUPPORT)
    file(GLOB __usb_src ${CMAKE_CURRENT_LIST_DIR}/usb/*.cc ${CMAKE_CURRENT_LIST_DIR}/usb/*.c)
    # UART bridge 仅在 MEGUMINLINK_USB_UART_BRIDGE_ENABLE 开启时编译
    if(NOT CONFIG_MEGUMINLINK_USB_UART_BRIDGE_ENABLE)
        list(REMOVE_ITEM __usb_src ${CMAKE_CURRENT_LIST_DIR}/usb/uart_bridge.cc)
    endif()
    list(APPEND __src ${__usb_src})
endif()

# CMSIS-DAP 固件（移植自 Arm CMSIS-DAP V2.1.2）：无条件编译。
# Local 模式（USB 本地执行）与无线接收端（target 侧执行 DAP 命令）
# 都依赖 DAP 核心；无线接收端通常是无 USB 的目标（如 ESP32-C3）。
file(GLOB __dap_src ${CMAKE_CURRENT_LIST_DIR}/cmsis_dap/Source/*.c)
list(APPEND __src ${__dap_src})

# LED 指示灯模块：LED 驱动被选中时编译（choice 非 NONE）；Disabled 时整个模块不参与构建。
# 各驱动源文件仅在对应的 CONFIG 选项选中时加入（led_controller.cc 为门面，任何
# 非 NONE 配置都需要）。
if(NOT CONFIG_MEGUMINLINK_LED_DRIVER_NONE)
    list(APPEND __src ${CMAKE_CURRENT_LIST_DIR}/led/led_controller.cc)
    if(CONFIG_MEGUMINLINK_LED_DRIVER_GPIO)
        list(APPEND __src ${CMAKE_CURRENT_LIST_DIR}/led/gpio_led_driver.cc)
    endif()
    if(CONFIG_MEGUMINLINK_LED_DRIVER_WS2812)
        list(APPEND __src ${CMAKE_CURRENT_LIST_DIR}/led/ws2812_led_driver.cc)
    endif()
endif()

idf_component_register(SRCS ${__src}
                    INCLUDE_DIRS "./include" "./cmsis_dap/Include" "./cmsis_dap/Config")
