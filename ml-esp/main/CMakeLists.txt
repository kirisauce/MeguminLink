file(GLOB __src ${CMAKE_CURRENT_LIST_DIR}/*.cc ${CMAKE_CURRENT_LIST_DIR}/*.c)

# main/usb/ 子目录（TinyUSB + UART 桥）仅在 CONFIG_MEGUMINLINK_USB_SUPPORT 开启时编译，
# 该选项仅对 ESP32-S3 可用（见同目录 Kconfig.projbuild）
if(CONFIG_MEGUMINLINK_USB_SUPPORT)
    file(GLOB __usb_src ${CMAKE_CURRENT_LIST_DIR}/usb/*.cc ${CMAKE_CURRENT_LIST_DIR}/usb/*.c)
    # UART bridge 仅在 MEGUMINLINK_USB_UART_BRIDGE_ENABLE 开启时编译
    if(NOT CONFIG_MEGUMINLINK_USB_UART_BRIDGE_ENABLE)
        list(REMOVE_ITEM __usb_src ${CMAKE_CURRENT_LIST_DIR}/usb/uart_bridge.cc)
    endif()
    list(APPEND __src ${__usb_src})

    # CMSIS-DAP 固件（移植自 Arm CMSIS-DAP V2.1.2；无 USB 则无传输通道，不编译）
    file(GLOB __dap_src ${CMAKE_CURRENT_LIST_DIR}/cmsis_dap/Source/*.c)
    list(APPEND __src ${__dap_src})
endif()

# LED 指示灯模块：仅在 GPIO 驱动被选中时编译（Disabled 时整个模块不参与构建）
if(CONFIG_MEGUMINLINK_LED_DRIVER_GPIO)
    file(GLOB __led_src ${CMAKE_CURRENT_LIST_DIR}/led/*.cc)
    list(APPEND __src ${__led_src})
endif()

idf_component_register(SRCS ${__src}
                    INCLUDE_DIRS "./include" "./cmsis_dap/Include" "./cmsis_dap/Config")
