file(GLOB __src ${CMAKE_CURRENT_LIST_DIR}/*.cc ${CMAKE_CURRENT_LIST_DIR}/*.c)

# usb.cc (TinyUSB) 仅在 CONFIG_MEGUMINLINK_USB_SUPPORT 开启时编译，
# 该选项仅对 ESP32-S3 可用（见同目录 Kconfig.projbuild）
if(NOT CONFIG_MEGUMINLINK_USB_SUPPORT)
    list(REMOVE_ITEM __src ${CMAKE_CURRENT_LIST_DIR}/usb.cc)
endif()

idf_component_register(SRCS ${__src}
                    INCLUDE_DIRS "./include")
