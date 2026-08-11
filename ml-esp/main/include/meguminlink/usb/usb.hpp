#pragma once
#ifndef __MEGUMINLINK_USB_HPP__
#define __MEGUMINLINK_USB_HPP__

#include <esp_err.h>
#include <freertos/FreeRTOS.h>

#include "meguminlink/common.hpp"

namespace mlink
{
/**
 * CMSIS-DAP v2 USB 设备。
 *
 * 仅负责描述符与 TinyUSB 驱动初始化；DAP 命令处理等逻辑后续在此类中实现。
 */
class CmsisDapUsb
{
  public:
    CmsisDapUsb();

    /** 安装 TinyUSB 驱动并注册 CMSIS-DAP v2 描述符（幂等） */
    esp_err_t init();

  private:
    esp_err_t uart_bridge_init();
    static void uart_bridge_task(void *arg);

    bool m_initialized = false;
    char m_serial[18]; // MACSTR 格式 "aa:bb:cc:dd:ee:ff"（17 字符 + NUL）
    const char *m_string_descriptors[5];
    QueueHandle_t m_uart_queue;

    static constexpr u16 USB_VID = 0x303A;
    static constexpr u16 USB_PID = 0x4020;
};
} // namespace mlink

#endif
