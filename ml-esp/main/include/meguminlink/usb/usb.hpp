#pragma once
#ifndef __MEGUMINLINK_USB_HPP__
#define __MEGUMINLINK_USB_HPP__

#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/ringbuf.h>

#include "meguminlink/common.hpp"
#include "sdkconfig.h"

namespace mlink
{
/**
 * CMSIS-DAP v2 USB 设备。
 *
 * 仅负责描述符与 TinyUSB 驱动初始化；DAP 命令处理在 usb.cc 的 dap_task 中。
 */
class CmsisDapUsb
{
  public:
    CmsisDapUsb();
    ~CmsisDapUsb();

    /** 安装 TinyUSB 驱动并注册 CMSIS-DAP v2 描述符（幂等） */
    esp_err_t init();

    /**
     * 启用/禁用整个 USB 设备（任务上下文调用）。
     *
     * disabled：tud_disconnect() 断开 USB 总线（主机端表现为拔线），
     * 并挂起 DAP/UART 桥处理任务；enabled：tud_connect() 重新枚举并恢复任务。
     * 用于无线调试模式下完全停用本地 USB 通路。
     */
    void set_enabled(bool enabled);

    TaskHandle_t m_dap_task = nullptr;
    RingbufHandle_t m_bulk_rx_buffer;

  private:
#if CONFIG_MEGUMINLINK_USB_UART_BRIDGE_ENABLE
    esp_err_t uart_bridge_init();
    static void uart_bridge_task(void *arg);
#endif

    bool m_initialized = false;
    char m_serial[18]; // MACSTR 格式 "aa:bb:cc:dd:ee:ff"（17 字符 + NUL）
    const char *m_string_descriptors[5];

#if CONFIG_MEGUMINLINK_USB_UART_BRIDGE_ENABLE
    QueueHandle_t m_uart_queue;
    TaskHandle_t m_uart_bridge_task = nullptr;
#endif

    static constexpr u16 USB_VID = 0x303A;
    static constexpr u16 USB_PID = 0x4020;
};
} // namespace mlink

#endif
