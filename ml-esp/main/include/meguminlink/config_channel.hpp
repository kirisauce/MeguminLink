#pragma once
#ifndef __MEGUMINLINK_CONFIG_CHANNEL_HPP__
#define __MEGUMINLINK_CONFIG_CHANNEL_HPP__

#include "meguminlink/common.hpp"
#include "sdkconfig.h"

#if CONFIG_MEGUMINLINK_USB_SUPPORT
#include <tusb.h> // tusb_control_request_t（EP0 厂商请求处理）
#endif

namespace mlink
{
/**
 * 配置/遥测通道：设备端统计计数器 + EP0 厂商请求（bRequest 0x4D）处理。
 *
 * 计数器是无锁 u32 累计值——热路径只多一次加法，32 位回绕由上位机对两次
 * 快照差分时处理（(cur - prev) & 0xFFFFFFFF）。计数器在所有目标上都累加
 * （无线接收端没有读取者，代价是几十字节 RAM），因此本模块在非 USB 目标
 * （如 ESP32-C3 接收端）上也参与构建，只有 EP0 协议部分按
 * CONFIG_MEGUMINLINK_USB_SUPPORT 裁剪——wireless_link.cc 的挂点因此无需 #if。
 *
 * 实例由 Application 持有（见 Application::config_channel）。
 * 协议细节见 config_channel.cc 顶部；上位机见 tools/mlinkctl.py。
 */
class ConfigChannel
{
  public:
    /** DAP 本地执行路径：一条命令的请求/响应字节（usb.cc 的响应宿调用） */
    void add_dap_command(u16 request_bytes, u16 response_bytes);

    /** ESP-NOW 一帧交给驱动发送（wireless_link.cc 的 send 调用） */
    void add_wireless_tx(u16 bytes);

    /** ESP-NOW 一帧接收（wireless_link.cc 的 recv_cb 调用，rssi 取自 rx_ctrl） */
    void add_wireless_rx(u16 bytes, i8 rssi);

    /** Local 模式 UART 桥：CDC → UART */
    void add_uart_rx(u16 bytes);

    /** Local 模式 UART 桥：UART → CDC */
    void add_uart_tx(u16 bytes);

    /** Wireless 模式 UART 桥：CDC → 无线 */
    void add_uart_wireless_rx(u16 bytes);

    /** Wireless 模式 UART 桥：无线 → CDC */
    void add_uart_wireless_tx(u16 bytes);

#if CONFIG_MEGUMINLINK_USB_SUPPORT
    /**
     * EP0 厂商控制请求（bRequest = 0x4D）处理。
     *
     * 返回 true = 本模块接管该请求（协议栈不应再按 vendor 类分派）；
     * false = 不是本模块的请求，交回调用方（未识别的请求最终 STALL）。
     * 只读命令走 IN 方向、无 DATA OUT 阶段，命令码在 wValue。
     */
    bool handle_control(u8 rhport, u8 stage, const tusb_control_request_t *request);
#endif

  private:
#if CONFIG_MEGUMINLINK_USB_SUPPORT
    /** 组装一帧响应到 m_response，返回整帧长度 */
    u16 build_response(u8 cmd, u8 seq);
#endif

    // 写入端各在自己的任务上下文串行（DAP 任务 / esp_wifi 任务 / 桥任务），
    // 读取端只有 EP0 请求处理，无需临界区。
    volatile u32 m_dap_cmds = 0;
    volatile u32 m_dap_request_bytes = 0;
    volatile u32 m_dap_response_bytes = 0;
    volatile u32 m_wireless_tx_bytes = 0;
    volatile u32 m_wireless_rx_bytes = 0;
    volatile i8 m_wireless_rssi = 0;
    volatile u32 m_uart_rx_bytes = 0;
    volatile u32 m_uart_tx_bytes = 0;
    volatile u32 m_uart_wireless_rx_bytes = 0;
    volatile u32 m_uart_wireless_tx_bytes = 0;

#if CONFIG_MEGUMINLINK_USB_SUPPORT
    /** 响应缓冲必须常驻：tud_control_xfer 只保存指针，DATA 阶段仍会回读 */
    u8 m_response[256];
#endif
};
} // namespace mlink

#endif // __MEGUMINLINK_CONFIG_CHANNEL_HPP__
