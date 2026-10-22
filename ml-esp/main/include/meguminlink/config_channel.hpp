#pragma once
#ifndef __MEGUMINLINK_CONFIG_CHANNEL_HPP__
#define __MEGUMINLINK_CONFIG_CHANNEL_HPP__

#include "meguminlink/common.hpp"
#include "meguminlink/wireless/frame.hpp"
#include "sdkconfig.h"

#include "meguminlink/wireless/arq_link.hpp"

// 存活探测状态用 portMUX_TYPE 保护，两种目标（含无 USB 的接收端）都要编译
#include "freertos/FreeRTOS.h" // IWYU pragma: keep

#if CONFIG_MEGUMINLINK_USB_SUPPORT
#include "meguminlink/usb/usb_capability.hpp"
#include <tusb.h>
#endif

namespace mlink
{
/**
 * 配置/遥测通道：设备端统计计数器 + EP0 厂商请求（bRequest 0x4D）处理。
 *
 * 计数器与线编码合成一组由 m_stats_lock 保护的累计值：热路径只多一次短临界区
 * 内的加法，32 位回绕由上位机对两次快照差分时处理（(cur - prev) & 0xFFFFFFFF）。
 * GetStats 在锁内取一份整体快照、锁外组帧——逐字段裸读会拼出不同时刻的组合值，
 * 而计数器是读改写累加，同一字段有多个写者时还会丢更新。计数器在所有目标上都
 * 累加（无线接收端没有读取者，代价是几十字节 RAM），因此本模块在非 USB 目标
 * （如 ESP32-C3 接收端）上也参与构建，只有 EP0 协议部分按
 * CONFIG_MEGUMINLINK_USB_SUPPORT 裁剪——wireless/link.cc 的挂点因此无需 #if。
 *
 * 实例由 Application 持有（见 Application::config_channel）。
 * 协议细节见 config_channel.cc 顶部；上位机见 tools/mlinkctl.py。
 */
class ConfigChannel
{
  public:
    /**
     * 一条 DAP 命令的请求/响应字节。usb.cc 在两处调用，口径一致（只计已完成
     * 的一轮往返）：本地执行路径的响应宿，以及无线转发路径转发成功之后。
     */
    void add_dap_command(u16 request_bytes, u16 response_bytes);

    /** ESP-NOW 一帧交给驱动发送（wireless/link.cc 的 send 调用） */
    void add_wireless_tx(u16 bytes);

    /** ESP-NOW 一帧接收（wireless/link.cc 的 recv_cb 调用，rssi 取自 rx_ctrl） */
    void add_wireless_rx(u16 bytes, i8 rssi);

    /** Local 模式 UART 桥：本机 UART RX 引脚收到的字节 */
    void add_uart_rx(u16 bytes);

    /** Local 模式 UART 桥：本机 UART TX 引脚发出的字节 */
    void add_uart_tx(u16 bytes);

    /** Wireless 模式 UART 桥：空口写回本机的串口字节 */
    void add_uart_wireless_rx(u16 bytes);

    /** Wireless 模式 UART 桥：本机发往空口的串口字节 */
    void add_uart_wireless_tx(u16 bytes);

#if CONFIG_MEGUMINLINK_WIRELESS_LOSS_SIM_ENABLE
    /**
     * 一次丢包注入判定（wireless/link.cc 的 send 调用）：每个过白名单的 DAP /
     * 串口数据出向帧都记一次，dropped 另计。
     *
     * 两者之比即实际注入比例。注入是概率性的，"没注入"与"注入了但被协议掩盖"
     * 单看现象分不出来，因此这两个数是该项功能唯一的自证手段。
     */
    void add_loss_frame(bool dropped);
#endif

    /**
     * 主机下发的线编码（TinyUSB 的 LINE_CODING_CHANGED 回调）。
     *
     * 各参数按 CDC 原值记录，不做语义转换：stop_bits 0=1、1=1.5、2=2；
     * parity 0=无、1=奇、2=偶、3=Mark、4=Space；data_bits 为 5-8。
     * 串口是否被主机打开不在这里缓存，GetStats 时实时查 TinyUSB 的 DTR
     * （见 CdcAcm::host_open），避免拔插后留下过期状态。
     * 四个字段与统计计数同组、在同一临界区内整体写入，快照因此不会读到改了一半的组合。
     */
    void set_uart_line_coding(u32 baud, u8 data_bits, u8 stop_bits, u8 parity);

    /**
     * 对端存活探测（Ping/Pong 空口往返，见 wireless/frame.hpp 的 payload::Ping）。
     *
     * 探测由上位机定时下发（EP0 命令 0x18；WebUI 在轮询里按固定间隔发、CLI 用
     * ping 子命令发），设备不自主发起：没有上位机时链路保持静默。设备侧负责
     * 回应、测量与结论——收到 Ping 回 Pong（主循环分派），本机收到 Pong 时结算
     * 往返时延，超过超时窗口未收到则判离线。结果由 GetStats 的 peer_online /
     * ping_rtt_us / ping_ok / ping_fail 上报（上位机见 tools/mlinkctl.py 与
     * tools/mlink-webui/src/lib/protocol.ts）。
     *
     * 收发都在主循环任务上下文：ESP-NOW 接收回调只投递、不回包（回调要短且
     * 不可重入驱动），代价是往返时延里含一次主循环调度（空闲时 <1ms）。
     * 探测状态机在两种目标上都编译——USB 侧（S3）是被探测方也是观察者，无 USB
     * 侧（C3）同样应答，双方行为对称，不为省一帧引入按目标裁剪的分叉。
     */
    struct PingSnapshot
    {
        u8 online;  /**< 0=从未探测 1=最近一次在线 2=最近一次离线 */
        u32 rtt_us; /**< 最近一次成功往返（微秒；从未成功时为 0） */
        u32 ok;     /**< 累计成功次数 */
        u32 fail;   /**< 累计失败次数（超时 / 未配对 / 发送失败） */
    };

    /**
     * 发起一次探测（上位机下发的 0x18 动作调用）。
     * 已有在途探测时忽略：上一次还没结算就不再叠加请求。
     */
    void start_ping();

    /**
     * 收到 Pong 帧（主循环分派）时的结算：来源 MAC 与回显 token 双重校验，
     * 只有本轮在途探测的回显才算数（迟到的旧回显直接丢弃）。
     */
    void on_pong(const u8 *src_mac, u32 token);

    /**
     * 主循环每拍调用：结算超过超时窗口的在途探测（判离线并累计 fail）。
     * 探测的发起不在这里——时机由上位机决定。
     */
    void poll_ping_timeout();

#if CONFIG_MEGUMINLINK_USB_SUPPORT
    /**
     * EP0 厂商控制请求（bRequest = 0x4D）处理。
     *
     * 返回 true = 本模块接管该请求（协议栈不应再按 vendor 类分派）；
     * false = 不是本模块的请求，交回调用方（未识别的请求最终 STALL）。
     * 读命令与动作命令为 IN 方向（命令码与参数在 wValue 里，响应在 DATA IN
     * 阶段回一帧）；SetUsbConfig 是唯一的 OUT 命令（载荷在 DATA OUT 阶段收，
     * 见 config_channel.cc 顶部协议注释）。
     */
    bool handle_control(u8 rhport, u8 stage, const tusb_control_request_t *request);
#endif

    /**
     * 待执行动作。控制命令只投递不执行：NVS 提交、esp_wifi 操作与重启
     * 都不能占用 tinyusb 任务，统一由主循环消费执行。
     *
     * 两端共用：无 USB 侧通过 RelayedUsbControl 收到同样的命令，投递的也是
     * 这些动作，因此动作集的语义在两端一致。
     */
    enum class Action : u8
    {
        None,           // 空槽
        SetMode,        // arg: 0=Local 1=Wireless
        EnterPairing,   // 进入配对流程（无线模式下有效）
        ExitPairing,    // 退出配对流程（无线模式且配对进行中有效）
        ClearPair,      // 清除配对记录并进入配对流程
        Reset,          // 恢复出厂设置：清除配对、模式与 USB 能力记录后重启
        Reboot,         // 立即重启
#if CONFIG_MEGUMINLINK_USB_SUPPORT
        EnterUpdate,   // 请求软复位进入更新模式（仅 USB 目标可达，见 update_mode.hpp）
#endif
        ApplyUsbConfig, // 落盘 USB 能力草稿并应用（重建描述符 + 重新枚举 USB）
        Ping,           // 立即探测已配对对端（存活与往返时延）
        SetWifiRate,    // arg: wifi_phy_rate_t 原值（ESP-NOW 发送速率档位）
        SetUartReliable, // arg: 0=串口出向走尽力而为 1=走可靠传输（落盘）
        SetLinkConfig,  // 落盘并应用可靠传输参数（草稿槽已由 SetLinkConfig 命令填好）
#if CONFIG_MEGUMINLINK_WIRELESS_LOSS_SIM_ENABLE
        SetLoss,        // arg: 千分比（DAP/UART 出向帧的注入丢包率，见 wireless/link.hpp）
#endif
    };

    /**
     * 投递待执行动作（EP0 请求或无线转发请求的上下文调用）。
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

    /**
     * 取走待应用的可靠传输参数草稿并清空槽（主循环执行 SetLinkConfig 时调用）。
     *
     * 参数由主机经 EP0 OUT 下发（无 USB 侧没有 EP0，只能靠对端在会话重置帧里
     * 捎带过来），解析校验在 DATA 阶段完成，这里只取结果。两端都编译：主循环的
     * 动作分派是共用的。
     */
    bool take_link_config_draft(ArqSettings *settings);

    /**
     * 收到对端转发的 USB 控制请求（无线接收回调调用）。
     *
     * 无 USB 侧的设置只能这样下发：本函数用与本机 USB 侧完全相同的命令表
     * 解释请求，因此两端语义不会漂移。只做解释与投递，不执行——命令可能
     * 操作 WiFi 驱动，而接收回调不可重入驱动，实际执行留给主循环。
     *
     * 响应在本目标被丢弃：转发是尽力而为的单向下发，发起端不等回执。
     */
    void on_relayed_control(const payload::RelayedUsbControl &req);

#if CONFIG_MEGUMINLINK_USB_SUPPORT
    /**
     * 取走待应用的 USB 能力草稿并清空槽（主循环执行 ApplyUsbConfig 时调用）。
     *
     * SetUsbConfig 只写草稿不落盘：校验失败的草稿在此处按"无草稿"处理
     * （取走后槽一律清空），主机已通过 GetUsbConfig 读到被拒原因。
     */
    bool take_usb_config_draft(UsbCapabilitySettings *settings);

    /**
     * 把一条本机刚接受的配置命令原样转发给对端执行（无 USB 侧的设置下发
     * 通路）。尽力而为：未配对或发送失败只记日志——本机设置已经生效，
     * 重新下发一次即可补偿。
     */
    void relay_to_peer(u8 cmd, u8 arg);
#endif

  private:
    /** 空 TLV 区的响应帧（动作命令与错误状态都用它） */
    u16 build_empty_response(u8 cmd, u8 seq, u8 status);

    /** 投递动作并按结果回帧：ok=已投递，busy=槽被占用 */
    u16 post_action_response(u8 cmd, u8 seq, Action action, u8 arg);

    /**
     * 执行一条配置通道命令，响应帧写入 m_response（命令表见 config_channel.cc）。
     *
     * 本机 USB 侧（EP0）与转发到对端的控制请求共用本函数，两端因此跑同一张
     * 命令表，不会出现"一端认识、另一端不认识"的分叉。只解释与投递动作，
     * 不做实际操作（执行在主循环）。
     *
     * @return 响应帧长度（容量不足时退化为 internal 状态的空帧）
     */
    u16 dispatch_control(u8 cmd, u8 arg, u8 seq);

#if CONFIG_MEGUMINLINK_USB_SUPPORT
    /** TLV 区填到 end 后收尾：写帧头、回整帧长度 */
    u16 finish_response(u8 cmd, u8 seq, usize end);

    /** 组装 GetStats 响应帧到 m_response，返回整帧长度 */
    u16 build_stats_response(u8 cmd, u8 seq);

    /** 组装 GetConfig 响应帧到 m_response，返回整帧长度 */
    u16 build_config_response(u8 cmd, u8 seq);

    /**
     * SetLinkConfig（OUT 方向）的处理：SETUP 阶段声明接收缓冲，DATA 阶段解析 TLV
     * 并写入草稿槽 + 投递动作。
     *
     * @return true=已接管；false=不是本命令或载荷格式非法（调用方 STALL）
     */
    bool handle_link_config_out(u8 rhport, u8 stage, const tusb_control_request_t *request);

    /** 解析 SetLinkConfig 载荷并写入草稿槽（格式非法时不改草稿并返回 false） */
    bool stage_link_config_draft(const u8 *data, usize len);

    /**
     * SetUsbConfig（OUT 方向）的处理：SETUP 阶段声明接收缓冲，DATA 阶段解析
     * TLV 并写入草稿槽。
     *
     * @return true=已接管；false=不是本命令或载荷格式非法（调用方 STALL）
     */
    bool handle_usb_config_out(u8 rhport, u8 stage, const tusb_control_request_t *request);

    /** 解析 SetUsbConfig 载荷并写入草稿槽（格式非法时不改草稿并返回 false） */
    bool stage_usb_config_draft(const u8 *data, usize len);

    /** 组装 GetUsbConfig 响应帧到 m_response，返回整帧长度 */
    u16 build_usb_config_response(u8 cmd, u8 seq);

    /** 草稿状态：0=无草稿 1=合法待应用 2=被拒（GetUsbConfig 的 USB_DRAFT） */
    u8 usb_draft_state() const;

    /** 被拒草稿的原因（无草稿或被拒时的残留状态由 state 表达） */
    UsbConfigReject usb_draft_reject() const;

    /** 槽内是否有合法草稿（ApplyUsbConfig 的准入条件） */
    bool has_valid_usb_draft() const;
#endif

    /** 结算一次探测：`m_ping_pending` 复位、结论与累计计数落库 */
    void finish_ping(bool online, u32 rtt_us);

    /** 读一次探测结果快照（组装 GetStats 响应时调用） */
    PingSnapshot ping_snapshot() const;

    /**
     * 统计与线编码的整组数据。
     *
     * 写入端分散在多个任务（DAP 任务 / esp_wifi 任务 / 桥任务 / 主循环），
     * 读取端是 EP0 请求处理；计数器是读改写累加，同一计数器会有多个写者，
     * 因此整组由 m_stats_lock 保护——组内字段要么一起被取走，要么都不取。
     */
    struct Stats
    {
        u32 dap_cmds = 0;
        u32 dap_request_bytes = 0;
        u32 dap_response_bytes = 0;
        u32 wireless_tx_bytes = 0;
        u32 wireless_rx_bytes = 0;
        i8 wireless_rssi = 0;
        u32 uart_rx_bytes = 0;
        u32 uart_tx_bytes = 0;
        u32 uart_wireless_rx_bytes = 0;
        u32 uart_wireless_tx_bytes = 0;
#if CONFIG_MEGUMINLINK_WIRELESS_LOSS_SIM_ENABLE
        u32 loss_frames = 0;  /**< 参与注入判定的出向帧数（比例的分母） */
        u32 loss_dropped = 0; /**< 被判丢的帧数（分子） */
#endif
        // 主机下发的线编码（CDC SET_LINE_CODING）。无回调时是桥接默认值 8N1，
        // 与 UartBridge 安装驱动时的初值一致；串口打开状态不缓存（见 set_uart_line_coding）。
        u32 uart_baud = CONFIG_MEGUMINLINK_UART_BRIDGE_BAUD_RATE;
        u8 uart_data_bits = 8;
        u8 uart_stop_bits = 0; // CDC_LINE_CODING_STOP_BITS_1
        u8 uart_parity = 0;    // CDC_LINE_CODING_PARITY_NONE
    };

    /** 取一份自洽的统计+线编码快照（锁内整体拷贝，TLV 组帧在锁外做） */
    Stats stats_snapshot() const;

    mutable portMUX_TYPE m_stats_lock = portMUX_INITIALIZER_UNLOCKED;
    Stats m_stats = {};

    /**
     * 存活探测状态：主循环发起/结算、收到 Pong 时校验，EP0 只读快照，
     * 三处并发，统一由 m_ping_lock 保护。token 单调递增，回显必须匹配
     * 当前在途 token，避免把上一轮超时后才到达的回显算成本轮结果。
     */
    mutable portMUX_TYPE m_ping_lock = portMUX_INITIALIZER_UNLOCKED;
    bool m_ping_pending = false;
    u32 m_ping_token = 0;
    i64 m_ping_sent_us = 0;     /**< 本轮发送时刻（esp_timer 微秒） */
    i64 m_ping_deadline_us = 0; /**< 本轮判定离线的截止时刻 */
    u8 m_ping_online = 0;       /**< PingSnapshot::online */
    u32 m_ping_rtt_us = 0;
    u32 m_ping_ok = 0;
    u32 m_ping_fail = 0;

    /**
     * 响应缓冲：dispatch_control 统一写在这里。
     * USB 侧必须常驻（tud_control_xfer 只保存指针，DATA 阶段仍会回读）；
     * 无 USB 侧只被转发的控制请求写到，内容随即丢弃，但两端共用同一份
     * 组装代码，不留分叉。
     */
    u8 m_response[256];

    /**
     * 可靠传输参数草稿单槽（EP0 OUT 的 DATA 阶段写、主循环执行 SetLinkConfig 时
     * 取走）。只接受到此为止：参数落盘与应用都在主循环做，不占用 tinyusb 任务。
     */
    ArqSettings m_link_draft = {};
    bool m_link_draft_present = false;
    mutable portMUX_TYPE m_link_draft_lock = portMUX_INITIALIZER_UNLOCKED;

    /** OUT 载荷接收缓冲：tud_control_xfer 只保存指针，DATA 阶段仍会回写 */
    u8 m_link_draft_buffer[128];

    /** 待执行动作单槽（请求上下文写、主循环读，portMUX 保护） */
    Action m_pending_action = Action::None;
    u8 m_pending_arg = 0;
    portMUX_TYPE m_action_lock = portMUX_INITIALIZER_UNLOCKED;

#if CONFIG_MEGUMINLINK_USB_SUPPORT
    /**
     * USB 能力草稿单槽（EP0 上下文写、主循环执行 ApplyUsbConfig 时读走）。
     * SetUsbConfig 的 OUT 载荷先落到 m_usb_draft_buffer，解析校验后存进本槽。
     */
    UsbCapabilitySettings m_usb_draft;
    bool m_usb_draft_present = false;
    bool m_usb_draft_rejected = false;
    UsbConfigReject m_usb_draft_reject = UsbConfigReject::None;
    mutable portMUX_TYPE m_usb_draft_lock = portMUX_INITIALIZER_UNLOCKED;

    /** OUT 载荷接收缓冲：tud_control_xfer 只保存指针，DATA 阶段仍会回写 */
    u8 m_usb_draft_buffer[256];
#endif
};
} // namespace mlink

#endif // __MEGUMINLINK_CONFIG_CHANNEL_HPP__
