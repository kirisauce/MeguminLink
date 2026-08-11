file(GLOB __src ${CMAKE_CURRENT_LIST_DIR}/*.cc ${CMAKE_CURRENT_LIST_DIR}/*.c)

# main/usb/ 子目录（TinyUSB + UART 桥）仅在 CONFIG_MEGUMINLINK_USB_SUPPORT 开启时编译，
# 该选项仅对 ESP32-S3 可用（见同目录 Kconfig.projbuild）
if(CONFIG_MEGUMINLINK_USB_SUPPORT)
    file(GLOB __usb_src ${CMAKE_CURRENT_LIST_DIR}/usb/*.cc ${CMAKE_CURRENT_LIST_DIR}/usb/*.c)
    list(APPEND __src ${__usb_src})
endif()

idf_component_register(SRCS ${__src}
                    INCLUDE_DIRS "./include")
