#pragma once
#ifndef __MEGUMINLINK_CONFIG_CHANNEL_HPP__
#define __MEGUMINLINK_CONFIG_CHANNEL_HPP__

#include "meguminlink/common.hpp"
#include "sdkconfig.h"

#if CONFIG_MEGUMINLINK_USB_SUPPORT
#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include <tusb.h>              // tusb_control_request_t（EP0 厂商请求处理）
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
     * 请求一律 IN 方向、无 DATA OUT 阶段：命令码与参数都在 SETUP 包的
     * wValue 里（低位命令码、高位参数），响应在 DATA IN 阶段回一帧。
     */
    bool handle_control(u8 rhport, u8 stage, const tusb_control_request_t *request);

    /**
     * 待执行动作。控制命令只投递不执行：NVS 提交、esp_wifi 操作与重启
     * 都不能占用 tinyusb 任务，统一由主循环消费执行。
     */
    enum class Action : u8
    {
        None,         // 空槽
        SetMode,      // arg: 0=Local 1=Wireless
        EnterPairing, // 进入配对流程（无线模式下有效）
        ExitPairing,  // 退出配对流程（无线模式且配对进行中有效）
        ClearPair,    // 清除配对记录并进入配对流程
        Reset,        // 清除配对与模式记录后重启
        Reboot,       // 立即重启
    };

    /**
     * 投递待执行动作（EP0 请求上下文调用）。
     * @return true=已接受；false=槽内已有未取走的动作（调用方回 busy）
     */
    bool post_action(Action action, u8 arg = 0);

    /**
     * 取走待执行动作并清空槽（主循环调用）。
     * @param action 输出：取到的动作，无动作时为 Action::None
     * @param arg    输出：动作参数，可为 nullptr
     * @return true=取到动作；false=当前无动作
     */
    bool take_action(Action *action, u8 *arg = nullptr);
#endif

  private:
#if CONFIG_MEGUMINLINK_USB_SUPPORT
    /** 空 TLV 区的响应帧（动作命令与错误状态都用它） */
    u16 build_empty_response(u8 cmd, u8 seq, u8 status);

    /** TLV 区填到 end 后收尾：写帧头、回整帧长度 */
    u16 finish_response(u8 cmd, u8 seq, usize end);

    /** 组装 GetStats 响应帧到 m_response，返回整帧长度 */
    u16 build_stats_response(u8 cmd, u8 seq);

    /** 组装 GetConfig 响应帧到 m_response，返回整帧长度 */
    u16 build_config_response(u8 cmd, u8 seq);

    /** 投递动作并按结果回帧：ok=已投递，busy=槽被占用 */
    u16 post_action_response(u8 cmd, u8 seq, Action action, u8 arg);
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

    /** 待执行动作单槽（EP0 上下文写、主循环读，portMUX 保护） */
    Action m_pending_action = Action::None;
    u8 m_pending_arg = 0;
    portMUX_TYPE m_action_lock = portMUX_INITIALIZER_UNLOCKED;
#endif
};
} // namespace mlink

#endif // __MEGUMINLINK_CONFIG_CHANNEL_HPP__
