#pragma once
#ifndef __MEGUMINLINK_USB_HPP__
#define __MEGUMINLINK_USB_HPP__

#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/ringbuf.h>

#include <array>

#include "meguminlink/common.hpp"
#include "sdkconfig.h"

namespace mlink
{
enum class UsbStringIndex {
  LangId = 0,
  Manufacturer,
  Product,
  SerialNumber,
  DapInterfaceName,
#if CONFIG_MEGUMINLINK_USB_UART_BRIDGE_ENABLE
  UartBridgeInterfaceName,
#endif
#if CONFIG_MEGUMINLINK_USB_WEBUSB_CAPABILITY_ENABLE
  WebUsbLandingPage,
#endif
  Max,
};

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
     *
     * 注意：当前无调用方——USB 在 Local 与 Wireless 模式下都常驻启用
     * （Wireless 下 DAP 命令由 dap_task 无线转发，见 usb.cc），此接口保留
     * 以备将来按需启停（如省电、仅断开 CDC 等场景）。
     */
    void set_enabled(bool enabled);

    TaskHandle_t m_dap_task = nullptr;
    RingbufHandle_t m_bulk_rx_buffer;

  private:
#if CONFIG_MEGUMINLINK_USB_UART_BRIDGE_ENABLE
    esp_err_t uart_bridge_init();
    static void uart_bridge_task(void *arg);

  public:
    /**
     * 无线 UartData 帧到达 → 写入 CDC TX（recv_cb 直通调用，esp_wifi 任务
     * 上下文；tinyusb_cdcacm_write_queue/flush 为任务安全 API）。
     * 尽力而为：CDC TX FIFO 满时丢弃多余字节并告警。
     */
    void uart_data_to_cdc(const u8 *data, size_t len);

    /**
     * 本地 UART 桥任务启停（apply_mode 调用）：仅 Local 模式运行，
     * Wireless 模式下 S3 本地 UART1 无数据流，挂起任务避免空转。
     * 幂等：重复 suspend/resume 安全。
     */
    void set_uart_bridge_active(bool active);

  private:
#endif

    bool m_initialized = false;
    char m_serial[18]; // MACSTR 格式 "aa:bb:cc:dd:ee:ff"（17 字符 + NUL）
    std::array<const char*, (usize)UsbStringIndex::Max> m_string_descriptors;

#if CONFIG_MEGUMINLINK_USB_UART_BRIDGE_ENABLE
    QueueHandle_t m_uart_queue;
    TaskHandle_t m_uart_bridge_task = nullptr;
#endif
};
} // namespace mlink

#endif
