#include "meguminlink/config_channel.hpp"

#include "meguminlink/app.hpp"
#include "meguminlink/nvs_flash.hpp" // peer_mac_key + esp_now.h（ESP_NOW_ETH_ALEN）
#include "meguminlink/wireless/frame.hpp"
#include "meguminlink/wireless/link.hpp"

#include <esp_app_desc.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <sdkconfig.h>

#include <algorithm>
#include <cstring>

using namespace mlink;

static constexpr char TAG[] = "mlink::config_channel";

void ConfigChannel::add_dap_command(u16 request_bytes, u16 response_bytes)
{
    portENTER_CRITICAL(&m_stats_lock);
    m_stats.dap_cmds = m_stats.dap_cmds + 1;
    m_stats.dap_request_bytes = m_stats.dap_request_bytes + request_bytes;
    m_stats.dap_response_bytes = m_stats.dap_response_bytes + response_bytes;
    portEXIT_CRITICAL(&m_stats_lock);
}

void ConfigChannel::add_wireless_tx(u16 bytes)
{
    portENTER_CRITICAL(&m_stats_lock);
    m_stats.wireless_tx_bytes = m_stats.wireless_tx_bytes + bytes;
    portEXIT_CRITICAL(&m_stats_lock);
}

void ConfigChannel::add_wireless_rx(u16 bytes, i8 rssi)
{
    portENTER_CRITICAL(&m_stats_lock);
    m_stats.wireless_rx_bytes = m_stats.wireless_rx_bytes + bytes;
    m_stats.wireless_rssi = rssi;
    portEXIT_CRITICAL(&m_stats_lock);
}

void ConfigChannel::add_uart_rx(u16 bytes)
{
    portENTER_CRITICAL(&m_stats_lock);
    m_stats.uart_rx_bytes = m_stats.uart_rx_bytes + bytes;
    portEXIT_CRITICAL(&m_stats_lock);
}

void ConfigChannel::add_uart_tx(u16 bytes)
{
    portENTER_CRITICAL(&m_stats_lock);
    m_stats.uart_tx_bytes = m_stats.uart_tx_bytes + bytes;
    portEXIT_CRITICAL(&m_stats_lock);
}

void ConfigChannel::add_uart_wireless_rx(u16 bytes)
{
    portENTER_CRITICAL(&m_stats_lock);
    m_stats.uart_wireless_rx_bytes = m_stats.uart_wireless_rx_bytes + bytes;
    portEXIT_CRITICAL(&m_stats_lock);
}

void ConfigChannel::add_uart_wireless_tx(u16 bytes)
{
    portENTER_CRITICAL(&m_stats_lock);
    m_stats.uart_wireless_tx_bytes = m_stats.uart_wireless_tx_bytes + bytes;
    portEXIT_CRITICAL(&m_stats_lock);
}

#if CONFIG_MEGUMINLINK_WIRELESS_LOSS_SIM_ENABLE
void ConfigChannel::add_loss_frame(bool dropped)
{
    portENTER_CRITICAL(&m_stats_lock);
    m_stats.loss_frames = m_stats.loss_frames + 1;
    if (dropped)
    {
        m_stats.loss_dropped = m_stats.loss_dropped + 1;
    }
    portEXIT_CRITICAL(&m_stats_lock);
}
#endif

void ConfigChannel::set_uart_line_coding(u32 baud, u8 data_bits, u8 stop_bits, u8 parity)
{
    // 四个字段在同一临界区内整体写入：GetStats 的快照不会读到改了一半的组合
    portENTER_CRITICAL(&m_stats_lock);
    m_stats.uart_baud = baud;
    m_stats.uart_data_bits = data_bits;
    m_stats.uart_stop_bits = stop_bits;
    m_stats.uart_parity = parity;
    portEXIT_CRITICAL(&m_stats_lock);
}

ConfigChannel::Stats ConfigChannel::stats_snapshot() const
{
    portENTER_CRITICAL(&m_stats_lock);
    const Stats snapshot = m_stats;
    portEXIT_CRITICAL(&m_stats_lock);
    return snapshot;
}

// ---- 协议（固件与上位机的唯一契约，上位机见 tools/mlinkctl.py 与
//      tools/mlink-webui/src/lib/protocol.ts）----
//
// 传输：EP0 厂商控制请求，bRequest = 0x4D（'M'），命令码与参数走 wValue
// （低字节命令码 | 高字节参数）、wIndex = seq（原样回显）。
//   - 读命令与动作命令一律 IN 方向（bmRequestType = 0xC0：IN、vendor、
//     device recipient）：wLength = 期望响应长度，响应帧在 DATA IN 阶段回给
//     主机，一次往返拿到结果；
//   - SetUsbConfig 是本通道唯一的 OUT 命令（bmRequestType = 0x40）：wLength
//     = TLV 载荷长度，载荷在 DATA OUT 阶段送达，没有 DATA IN 阶段，因此它
//     不回状态帧（校验结果由随后的一次 GetUsbConfig 读取）。
// 不在上表内的厂商请求交回调用方（最终 STALL）。
//
// 帧头 8B（大端）：magic u16（0x4D4C，字节流恒为 'M' 'L'）+ cmd u8 + seq u8
// + len u16（TLV 区长度）+ status u8 + reserved u8，后接 len 字节 TLV 区。
// TLV：tag u8 | len u8 | value，len == 0xFF 表示扩展长度（后接 u16 大端）；
// 整数一律大端，字符串不带结尾 NUL。新增 tag 不影响旧上位机解析。
// SetUsbConfig 的载荷用同一套 TLV 编码，但不带帧头（命令与状态都在请求本身）。
//
// 命令表（0x01-0x0F 只读，0x10-0x1F 动作）：
//   0x01 GetStats       读统计快照
//   0x02 GetConfig      读设备身份与配置项
//   0x03 GetUsbConfig   读 USB 能力与设备名（生效值 + 草稿状态）
//   0x10 SetMode        参数 0=Local 1=Wireless，其他值回 malformed
//   0x11 EnterPairing   进入配对流程（无线模式下有效，Local 记日志忽略）
//   0x12 ClearPair      清除配对记录并进入配对流程
//   0x13 Reset          恢复出厂设置：清除配对、模式与 USB 能力记录后重启
//   0x14 Reboot         立即重启
//   0x15 ExitPairing    退出配对流程（无线模式且配对进行中有效）
//   0x16 SetUsbConfig   OUT：提交 USB 能力与设备名草稿，只校验不落盘不生效
//   0x17 ApplyUsbConfig 落盘草稿并应用（重建描述符 + 重新枚举 USB）
//   0x18 Ping           探测已配对对端（存活与往返时延，结果走 GetStats）
//   0x19 SetWifiRate    参数 = wifi_phy_rate_t 原值，ESP-NOW 发送速率档位
//   0x1B SetLinkConfig  OUT：可靠传输参数（TLV 载荷，tag 见 payload 注释）；参数
//                        由主机下发本机，本机再经会话重置帧同步给对端
//   0x1A SetLoss        参数 = 注入丢包率千分比 0-255（0.1% 步进），0 = 关闭；
//                        仅在 CONFIG_MEGUMINLINK_WIRELESS_LOSS_SIM_ENABLE 下存在
//   0x1C EnterUpdate   软复位进入更新模式：重启后 USB 只暴露一个 CDC 串口并
//                        显示渐变灯效，硬复位退出；仅 USB 目标支持（无 USB 侧回
//                        unsupported）。进入请求跨软复位传递，见 update_mode.cc
//   0x1D SetUartReliable 参数 0=关 1=开（串口出向是否走可靠传输，两端同时应用），
//                        其他值回 malformed
// 动作命令的响应 TLV 区为空：status == ok 表示动作已投递（执行在主循环，
// 完成与否看串口日志与设备行为），busy 表示槽内已有未取走的动作。
//
// 无 USB 侧（接收端）没有 EP0，它的设置由 USB 侧把整条控制请求经
// RelayedUsbControl 转发过来，在这里用同一张命令表执行（见
// ConfigChannel::on_relayed_control）。因此命令表在两端完全一致：标了
// "两端" 的命令由 USB 侧在接受的当下转发一次，只读命令（GetStats /
// GetConfig / GetUsbConfig）在无 USB 侧按 Unsupported 应答，因为那里没有
// 宿主可读的状态。
//
// USB 能力三阶段（0x16 → 0x03 → 0x17）：0x16 是 OUT 请求、回不了状态帧，
// 因此只做接收与校验；主机用 0x03 读出草稿状态（合法待应用 / 被拒 + 原因），
// 合法才发 0x17 落盘生效。载荷格式非法（缺项或 TLV 越界）在 0x16 的 DATA
// 阶段直接 STALL；内容非法（URL 或设备名超长、含非可打印字符、启用编译期未
// 支持的能力）留在草稿状态里由 0x03 报出。载荷里 WebUSB / WinUSB / landing
// page 三项必须齐全，HID 兼容模式与设备名是可选 tag（缺省沿用当前生效值）。
namespace
{
constexpr u8 VENDOR_CODE = 0x4D;
constexpr u16 FRAME_MAGIC = 0x4D4C;
constexpr usize FRAME_HEADER_SIZE = 8;

enum class Cmd : u8
{
    GetStats = 0x01,
    GetConfig = 0x02,
    GetUsbConfig = 0x03,
    SetMode = 0x10,
    EnterPairing = 0x11,
    ClearPair = 0x12,
    Reset = 0x13,
    Reboot = 0x14,
    ExitPairing = 0x15,
    SetUsbConfig = 0x16,
    ApplyUsbConfig = 0x17,
    Ping = 0x18,
    SetWifiRate = 0x19,
    SetLinkConfig = 0x1B,
    EnterUpdate = 0x1C,
    SetUartReliable = 0x1D,
#if CONFIG_MEGUMINLINK_WIRELESS_LOSS_SIM_ENABLE
    SetLoss = 0x1A,
#endif
};

enum class Status : u8
{
    Ok = 0x00,
    UnknownCommand = 0x01,
    Malformed = 0x02,
    Internal = 0x03,
    Unsupported = 0x04, // 命令已定义但本固件未实现
    Busy = 0x05,        // 上一个动作尚未被主循环取走
};

/** 控制请求 bmRequestType 的方向位（TUSB_DIR_IN_MASK；无 USB 侧不能引 tusb.h） */
constexpr u8 CONTROL_DIR_IN = 0x80;
} // namespace

/** 空 TLV 区的响应帧：长度、状态与命令回显都由调用方给定 */
u16 ConfigChannel::build_empty_response(u8 cmd, u8 seq, u8 status)
{
    m_response[0] = static_cast<u8>(FRAME_MAGIC >> 8);
    m_response[1] = static_cast<u8>(FRAME_MAGIC);
    m_response[2] = cmd;
    m_response[3] = seq;
    m_response[4] = 0;
    m_response[5] = 0;
    m_response[6] = status;
    m_response[7] = 0;
    return FRAME_HEADER_SIZE;
}

bool ConfigChannel::post_action(Action action, u8 arg)
{
    portENTER_CRITICAL(&m_action_lock);
    const bool free_slot = m_pending_action == Action::None;
    if (free_slot)
    {
        m_pending_arg = arg;
        m_pending_action = action;
    }
    portEXIT_CRITICAL(&m_action_lock);
    return free_slot;
}

bool ConfigChannel::take_action(Action *action, u8 *arg)
{
    portENTER_CRITICAL(&m_action_lock);
    const Action pending = m_pending_action;
    if (pending != Action::None)
    {
        if (arg != nullptr)
        {
            *arg = m_pending_arg;
        }
        m_pending_action = Action::None;
    }
    portEXIT_CRITICAL(&m_action_lock);

    *action = pending;
    return pending != Action::None;
}

u16 ConfigChannel::post_action_response(u8 cmd, u8 seq, Action action, u8 arg)
{
    if (!post_action(action, arg))
    {
        return build_empty_response(cmd, seq, static_cast<u8>(Status::Busy));
    }
    // 投递成功即唤醒主循环：动作不必等满一个主循环节拍
    xSemaphoreGive(Application::get_instance().main_task_wake());
    return build_empty_response(cmd, seq, static_cast<u8>(Status::Ok));
}

u16 ConfigChannel::dispatch_control(u8 cmd, u8 arg, u8 seq)
{
    switch (static_cast<Cmd>(cmd))
    {
    case Cmd::SetMode:
        // 参数只有 local/wireless 两个取值，其余按 malformed 拒绝且不占动作槽
        return arg <= static_cast<u8>(Mode::Wireless)
                   ? post_action_response(cmd, seq, Action::SetMode, arg)
                   : build_empty_response(cmd, seq, static_cast<u8>(Status::Malformed));
    case Cmd::EnterPairing:
        return post_action_response(cmd, seq, Action::EnterPairing, 0);
    case Cmd::ClearPair:
        return post_action_response(cmd, seq, Action::ClearPair, 0);
    case Cmd::Reset:
        return post_action_response(cmd, seq, Action::Reset, 0);
    case Cmd::Reboot:
        return post_action_response(cmd, seq, Action::Reboot, 0);
    case Cmd::ExitPairing:
        return post_action_response(cmd, seq, Action::ExitPairing, 0);
    case Cmd::Ping:
        return post_action_response(cmd, seq, Action::Ping, 0);
    case Cmd::SetWifiRate:
        // 档位取值域由无线链路定义（PHY 模式由档位推导）；非法值不占动作槽
        return mlink::wifi_rate_supported(arg)
                   ? post_action_response(cmd, seq, Action::SetWifiRate, arg)
                   : build_empty_response(cmd, seq, static_cast<u8>(Status::Malformed));
    case Cmd::SetUartReliable:
        // 只有开/关两个取值，其余按 malformed 拒绝且不占动作槽
        return arg <= 1 ? post_action_response(cmd, seq, Action::SetUartReliable, arg)
                        : build_empty_response(cmd, seq, static_cast<u8>(Status::Malformed));
#if CONFIG_MEGUMINLINK_WIRELESS_LOSS_SIM_ENABLE
    case Cmd::SetLoss:
        // 丢包率上限由无线链路定义；越界不占动作槽。未编译该功能时本命令码
        // 不存在，落到下面的 default 回 UnknownCommand。
        return arg <= mlink::MAX_LOSS_PERMILLE
                   ? post_action_response(cmd, seq, Action::SetLoss, arg)
                   : build_empty_response(cmd, seq, static_cast<u8>(Status::Malformed));
#endif
#if CONFIG_MEGUMINLINK_USB_SUPPORT
    case Cmd::GetStats:
        return build_stats_response(cmd, seq);
    case Cmd::GetConfig:
        return build_config_response(cmd, seq);
    case Cmd::GetUsbConfig:
        return build_usb_config_response(cmd, seq);
    case Cmd::SetUsbConfig:
    case Cmd::SetLinkConfig:
        // OUT 命令出现在 IN 方向：命令码有效但方向不对，按未识别命令处理
        return build_empty_response(cmd, seq, static_cast<u8>(Status::UnknownCommand));
    case Cmd::ApplyUsbConfig:
        // 没有合法草稿就没什么可应用的（被拒的草稿不算，原因由 GetUsbConfig 报出）
        return has_valid_usb_draft() ? post_action_response(cmd, seq, Action::ApplyUsbConfig, 0)
                                     : build_empty_response(cmd, seq, static_cast<u8>(Status::Malformed));
    case Cmd::EnterUpdate:
        // 只投递动作：置跨复位请求标志与重启都在主循环做（这里占的是 tinyusb 任务）
        return post_action_response(cmd, seq, Action::EnterUpdate, 0);
#else
    case Cmd::GetStats:
    case Cmd::GetConfig:
    case Cmd::GetUsbConfig:
    case Cmd::SetUsbConfig:
    case Cmd::ApplyUsbConfig:
    case Cmd::SetLinkConfig:
    case Cmd::EnterUpdate:
        // 只读命令与 USB 能力：本目标（无 USB 侧）没有宿主可读的状态可回
        return build_empty_response(cmd, seq, static_cast<u8>(Status::Unsupported));
#endif
    default:
        // 未定义的命令码：回带 UnknownCommand 状态的空帧（不回 STALL，
        // 让上位机能明确区分"设备不支持该命令"与"传输层失败"）
        return build_empty_response(cmd, seq, static_cast<u8>(Status::UnknownCommand));
    }
}

void ConfigChannel::on_relayed_control(const payload::RelayedUsbControl &req)
{
    if (req.b_request != VENDOR_CODE)
    {
        return; // 不是配置通道的请求：对端版本不符时静默忽略
    }

    // 只处理 IN 方向：命令码与参数在 wValue 里，一次往返即可完成。OUT 方向
    // 需要 USB 侧的 DATA 阶段状态机与草稿槽，本目标没有。
    if ((req.bm_request_type & CONTROL_DIR_IN) == 0)
    {
        ESP_LOGW(TAG, "Relayed OUT control 0x%02X ignored on this target", req.b_request);
        return;
    }

    const u8 cmd = static_cast<u8>(req.w_value & 0xFF);
    const u8 arg = static_cast<u8>(req.w_value >> 8);
    const u8 seq = static_cast<u8>(req.w_index & 0xFF);

    // 只解释与投递：命令可能操作 WiFi 驱动，而本函数运行在协议任务里，不能
    // 重入驱动。响应丢弃——转发是单向下发，发起端不等回执。
    const u16 len = dispatch_control(cmd, arg, seq);
    ESP_LOGI(TAG, "Relayed control 0x%02X (arg 0x%02X) dispatched, %u-byte response discarded", cmd, arg, len);
}

#if CONFIG_MEGUMINLINK_USB_SUPPORT

namespace
{
// tag 段位：0x01-0x0F 设备状态、0x10-0x1F DAP、0x20-0x2F 无线、0x30-0x3F UART、
// 0x40-0x4F 系统、0x50-0x5F 配置项、0x60-0x6F USB 能力。
enum : u8
{
    TAG_MODE = 0x01,             // u8：0=Local 1=Wireless
    TAG_PAIR = 0x02,             // u8：0=未配对 1=已配对 2=配对中
    TAG_RSSI = 0x03,             // i8：最近一次无线接收的 RSSI（dBm）
    TAG_DAP_CMDS = 0x10,         // u32：本地执行的 DAP 命令数
    TAG_DAP_REQUEST = 0x11,      // u32：DAP 请求字节数
    TAG_DAP_RESPONSE = 0x12,     // u32：DAP 响应字节数
    TAG_WIRELESS_TX = 0x20,      // u32：ESP-NOW 发送帧字节数
    TAG_WIRELESS_RX = 0x21,      // u32：ESP-NOW 接收帧字节数
    TAG_PEER_ONLINE = 0x22,      // u8：协议连接 1=已连接 2=未连接
    TAG_PING_RTT = 0x23,         // u32：协议层平滑往返时延（微秒）
#if CONFIG_MEGUMINLINK_WIRELESS_LOSS_SIM_ENABLE
    TAG_LOSS_FRAMES = 0x26,  // u32：参与丢包注入判定的出向帧数
    TAG_LOSS_DROPPED = 0x27, // u32：被注入丢弃的帧数
#endif
    TAG_UART_RX = 0x30,          // u32：Local 模式本机 UART RX 收到的字节数
    TAG_UART_TX = 0x31,          // u32：Local 模式本机 UART TX 发出的字节数
    TAG_UART_WIRELESS_RX = 0x32, // u32：Wireless 模式空口写回本机的字节数
    TAG_UART_WIRELESS_TX = 0x33, // u32：Wireless 模式本机发往空口的字节数
    TAG_UART_HOST_OPEN = 0x34,   // u8：主机是否已打开 CDC 串口（DTR）
    TAG_UART_BAUD = 0x35,        // u32：主机下发的波特率
    TAG_UART_DATA_BITS = 0x36,   // u8：数据位 5-8
    TAG_UART_STOP_BITS = 0x37,   // u8：停止位 0=1 1=1.5 2=2
    TAG_UART_PARITY = 0x38,      // u8：校验 0=无 1=奇 2=偶 3=Mark 4=Space
    TAG_UART_RELIABLE = 0x39,    // u8：串口出向是否走可靠传输 0=尽力而为 1=可靠
    // 可靠传输统计：无线段（0x20-0x2F）已排满，单开 0x70-0x7F 一段
    TAG_ARQ_STATE = 0x70,         // u8：bit0-3 DAP 通道窗口、bit4-6 在途帧数、bit7 固定 0
    TAG_ARQ_TX = 0x71,            // u32：首次发出的数据报数
    TAG_ARQ_RETX = 0x72,          // u32：数据报重传数
    TAG_ARQ_ACKED = 0x73,         // u32：被确认的数据报数
    TAG_ARQ_FAILED = 0x74,        // u32：固定 0（可靠消息不单条失败）
    TAG_ARQ_DUP = 0x75,           // u32：收到的重复数据报数
    TAG_ARQ_OOO = 0x76,           // u32：序号超出确认位图的帧数
    TAG_ARQ_SKIPPED = 0x77,       // u32：固定 0
    TAG_ARQ_REPLAYED = 0x78,      // u32：固定 0
    TAG_ARQ_REPLAY_MISS = 0x79,   // u32：固定 0
    TAG_ARQ_SRTT_MS = 0x7A,       // u32：平滑往返时延（毫秒）
    TAG_ARQ_RTO_MS = 0x7B,        // u32：当前基础 RTO（毫秒）
    TAG_ARQ_BREAKER_TRIPS = 0x7C, // u32：固定 0
    TAG_ARQ_TX_DROPPED = 0x7D,    // u32：发送队列满被拒的提交数
    TAG_ARQ_RX_DROPPED = 0x7E,    // u32：接收队列满被丢弃的帧数
    TAG_ARQ_CMD_DROPPED = 0x7F,   // u32：固定 0
    TAG_UPTIME = 0x40,           // u32：上电毫秒数（49.7 天回绕）
    TAG_FW_VERSION = 0x41,       // string：应用版本（esp_app_desc.version）
    TAG_CHIP_TARGET = 0x42,      // string：目标芯片（CONFIG_IDF_TARGET）
    TAG_IDF_VERSION = 0x43,      // string：ESP-IDF 版本（esp_app_desc.idf_ver）
    TAG_CFG_MODE = 0x50,         // u8：持久化的运行模式 0=Local 1=Wireless
    TAG_CFG_PEER_MAC = 0x51,     // blob 6B：已配对对端 MAC（未配对时不下发）
    TAG_CFG_WIFI_RATE = 0x52,    // u8：ESP-NOW 发送速率档位（wifi_phy_rate_t 原值）
    TAG_CFG_ARQ_RTO_MIN = 0x59,     // u16：初始 RTO（毫秒）
    TAG_CFG_ARQ_RTO_INIT = 0x5A,    // u16：初始 RTO（毫秒）
    TAG_CFG_ARQ_RTO_MAX = 0x5B,     // u16：RTO 上限（毫秒）
    TAG_CFG_ARQ_DELAYED_ACK = 0x5C, // u16：延迟确认（毫秒）
    TAG_CFG_ARQ_PROBE = 0x5D,       // u16：保活间隔（毫秒）
    // 0x55 未分配（保留号位）
#if CONFIG_MEGUMINLINK_WIRELESS_LOSS_SIM_ENABLE
    TAG_CFG_LOSS_PERMILLE = 0x53, // u8：注入丢包率千分比 0-255
#endif
    TAG_USB_SUPPORTED = 0x60,    // u8：编译期支持的能力位（bit0 WebUSB / bit1 WinUSB / bit2 HID 兼容模式）
    TAG_USB_WEBUSB = 0x61,       // u8：WebUSB 生效值 0/1
    TAG_USB_WINUSB = 0x62,       // u8：WinUSB 生效值 0/1
    TAG_USB_LANDING_PAGE = 0x63, // string：landing page URL（生效值）
    TAG_USB_DRAFT = 0x64,        // u8：草稿状态 0=无草稿 1=待应用 2=被拒
    TAG_USB_DRAFT_REASON = 0x65, // u8：被拒原因（UsbConfigReject）
    TAG_USB_HID_COMPAT = 0x66,   // u8：HID 兼容模式生效值 0/1（提交时可选，缺省沿用当前值）
    TAG_USB_DEVICE_NAME = 0x67,  // string：自定义设备名（生效值，空串=不用自定义名；提交时可选）
};

usize tlv_u8(u8 *out, u8 tag, u8 value)
{
    out[0] = tag;
    out[1] = 1;
    out[2] = value;
    return 3;
}

usize tlv_u16(u8 *out, u8 tag, u16 value)
{
    out[0] = tag;
    out[1] = 2;
    out[2] = static_cast<u8>(value >> 8);
    out[3] = static_cast<u8>(value);
    return 4;
}

usize tlv_i8(u8 *out, u8 tag, i8 value)
{
    return tlv_u8(out, tag, static_cast<u8>(value));
}

usize tlv_u32(u8 *out, u8 tag, u32 value)
{
    out[0] = tag;
    out[1] = 4;
    out[2] = static_cast<u8>(value >> 24);
    out[3] = static_cast<u8>(value >> 16);
    out[4] = static_cast<u8>(value >> 8);
    out[5] = static_cast<u8>(value);
    return 6;
}

/** 任意字节串（len < 0xFF：0xFF 是扩展长度标志，本次响应不涉及） */
usize tlv_blob(u8 *out, u8 tag, const u8 *value, usize len)
{
    out[0] = tag;
    out[1] = static_cast<u8>(len);
    memcpy(out + 2, value, len);
    return 2 + len;
}

/** C 字符串（不带结尾 NUL）；空指针按空串处理 */
usize tlv_str(u8 *out, u8 tag, const char *value)
{
    if (value == nullptr)
    {
        return tlv_blob(out, tag, nullptr, 0);
    }
    return tlv_blob(out, tag, reinterpret_cast<const u8 *>(value), strlen(value));
}

/** 0=未配对 1=已配对 2=配对中；配对流程进行中优先（此时 NVS 里可能仍有旧记录） */
u8 pair_state()
{
    auto &app = Application::get_instance();
    if (app.pairing_active())
    {
        return 2;
    }
    return peer_mac_key().cached() != nullptr ? 1 : 0;
}

/** SetUsbConfig 载荷的解析结果 */
struct ParsedUsbConfig
{
    /** 以当前生效配置为底：只出现在载荷里的项被覆盖 */
    UsbCapabilitySettings settings;
    /** landing page 的原始长度：超上限时草稿存截断值，但仍按原始长度报 TooLong */
    usize landing_page_len = 0;
    /** 设备名的原始长度：同样是超上限时按原始长度报 TooLong */
    usize device_name_len = 0;
    /** 三项（WebUSB / WinUSB / landing page）是否齐全；HID 兼容模式与设备名是可选 tag */
    bool complete = false;
};

/**
 * 解析 SetUsbConfig 的 TLV 载荷：WebUSB / WinUSB / landing page 必须给出，
 * HID 兼容模式与设备名可选，缺省沿用 base。
 *
 * @param[in] base 当前生效配置，作为草稿初值
 * @return true=格式合法；false=TLV 越界/缺项/取值长度不对（调用方在 DATA 阶段
 *         返回 false，由 TinyUSB 在状态阶段 STALL，主机端拿到传输失败）
 */
bool parse_usb_config(const u8 *data, usize len, const UsbCapabilitySettings &base, ParsedUsbConfig *out)
{
    out->settings = base;

    bool has_webusb = false;
    bool has_winusb = false;
    bool has_landing_page = false;

    usize pos = 0;
    while (pos + 2 <= len)
    {
        const u8 tag = data[pos];
        usize value_len = data[pos + 1];
        pos += 2;
        if (value_len == 0xFF)
        {
            if (pos + 2 > len)
            {
                return false;
            }
            value_len = (static_cast<usize>(data[pos]) << 8) | data[pos + 1];
            pos += 2;
        }
        if (pos + value_len > len)
        {
            return false;
        }

        const u8 *value = data + pos;
        pos += value_len;

        switch (tag)
        {
        case TAG_USB_WEBUSB:
            if (value_len != 1)
            {
                return false;
            }
            out->settings.webusb = value[0] != 0 ? 1 : 0;
            has_webusb = true;
            break;

        case TAG_USB_WINUSB:
            if (value_len != 1)
            {
                return false;
            }
            out->settings.winusb = value[0] != 0 ? 1 : 0;
            has_winusb = true;
            break;

        case TAG_USB_LANDING_PAGE: {
            out->landing_page_len = value_len;
            const usize copy_len = std::min(value_len, USB_LANDING_PAGE_MAX_LEN);
            memcpy(out->settings.landing_page, value, copy_len);
            out->settings.landing_page[copy_len] = '\0';
            has_landing_page = true;
            break;
        }

        case TAG_USB_HID_COMPAT:
            if (value_len != 1)
            {
                return false;
            }
            out->settings.hid_compat = value[0] != 0 ? 1 : 0;
            break;

        case TAG_USB_DEVICE_NAME: {
            // 空串是合法取值：表示不用自定义名。超长时草稿只留截断值，
            // 原始长度另记（见 stage_usb_config_draft 的 TooLong 判定）
            out->device_name_len = value_len;
            const usize copy_len = std::min(value_len, USB_DEVICE_NAME_MAX_LEN);
            memcpy(out->settings.device_name, value, copy_len);
            out->settings.device_name[copy_len] = '\0';
            break;
        }

        default:
            break; // 未知 tag：忽略，便于将来扩展
        }
    }

    if (pos != len)
    {
        return false; // 尾部有残留字节：不是合法的 TLV 区
    }

    out->complete = has_webusb && has_winusb && has_landing_page;
    return out->complete;
}
} // namespace

/** TLV 区填到 end 之后收尾：写帧头并回整帧长度（容量不足则退化为 internal） */
u16 ConfigChannel::finish_response(u8 cmd, u8 seq, usize end)
{
    if (end > sizeof(m_response))
    {
        // 缓冲容量不足（新增 tag 却忘了扩容量）：返回空 TLV 区而非越界
        ESP_LOGE(TAG, "Response buffer too small (%u > %u)", static_cast<unsigned>(end),
                 static_cast<unsigned>(sizeof(m_response)));
        return build_empty_response(cmd, seq, static_cast<u8>(Status::Internal));
    }

    const u16 payload_len = static_cast<u16>(end - FRAME_HEADER_SIZE);
    m_response[0] = static_cast<u8>(FRAME_MAGIC >> 8);
    m_response[1] = static_cast<u8>(FRAME_MAGIC);
    m_response[2] = cmd;
    m_response[3] = seq;
    m_response[4] = static_cast<u8>(payload_len >> 8);
    m_response[5] = static_cast<u8>(payload_len);
    m_response[6] = static_cast<u8>(Status::Ok);
    m_response[7] = 0;
    return static_cast<u16>(end);
}

u16 ConfigChannel::build_stats_response(u8 cmd, u8 seq)
{
    // 统计与线编码先取一份自洽快照：此后只读局部副本，TLV 组帧不接触统计锁
    const Stats stats = stats_snapshot();
    usize p = FRAME_HEADER_SIZE;

    p += tlv_u8(m_response + p, TAG_MODE, Application::get_instance().mode() == Mode::Local ? 0 : 1);
    p += tlv_u8(m_response + p, TAG_PAIR, pair_state());
    p += tlv_i8(m_response + p, TAG_RSSI, stats.wireless_rssi);
    p += tlv_u32(m_response + p, TAG_DAP_CMDS, stats.dap_cmds);
    p += tlv_u32(m_response + p, TAG_DAP_REQUEST, stats.dap_request_bytes);
    p += tlv_u32(m_response + p, TAG_DAP_RESPONSE, stats.dap_response_bytes);
    p += tlv_u32(m_response + p, TAG_WIRELESS_TX, stats.wireless_tx_bytes);
    p += tlv_u32(m_response + p, TAG_WIRELESS_RX, stats.wireless_rx_bytes);

    const WirelessLink &link = Application::get_instance().wireless_link();
    const meaproto::Stats protocol = link.stats();
    {
        // 可靠传输的运行状态：丢帧是否被吸收、恢复得多快，全看这几个数。
        // 窗口取 DAP 通道（配置默认窗口 16 装不进 4 位），未连接时按 0 上报
        u8 state = link.connected() ? static_cast<u8>(link.dap_window() & 0x0F) : 0;
        const u32 in_flight = protocol.datagrams_in_flight < 0x07 ? protocol.datagrams_in_flight : 0x07;
        state |= static_cast<u8>(in_flight << 4);
        p += tlv_u8(m_response + p, TAG_ARQ_STATE, state);
        p += tlv_u32(m_response + p, TAG_ARQ_TX, protocol.datagrams_sent);
        p += tlv_u32(m_response + p, TAG_ARQ_RETX, protocol.datagrams_retransmitted);
        p += tlv_u32(m_response + p, TAG_ARQ_ACKED, protocol.datagrams_acked);
        p += tlv_u32(m_response + p, TAG_ARQ_FAILED, 0);
        p += tlv_u32(m_response + p, TAG_ARQ_DUP, protocol.duplicate_datagrams);
        p += tlv_u32(m_response + p, TAG_ARQ_OOO, protocol.out_of_window);
        p += tlv_u32(m_response + p, TAG_ARQ_SKIPPED, 0);
        p += tlv_u32(m_response + p, TAG_ARQ_REPLAYED, 0);
        p += tlv_u32(m_response + p, TAG_ARQ_REPLAY_MISS, 0);
        p += tlv_u32(m_response + p, TAG_ARQ_SRTT_MS, protocol.rtt_ms);
        p += tlv_u32(m_response + p, TAG_ARQ_RTO_MS, protocol.rto_ms);
        p += tlv_u32(m_response + p, TAG_ARQ_BREAKER_TRIPS, 0);

        // 环满丢弃的证据：这两个数在正常链路上应当为 0——非 0 说明发送或接收
        // 队列跟不上（执行队列满的计数固定为 0）
        p += tlv_u32(m_response + p, TAG_ARQ_TX_DROPPED, protocol.tx_queue_dropped);
        p += tlv_u32(m_response + p, TAG_ARQ_RX_DROPPED, protocol.rx_queue_dropped);
        p += tlv_u32(m_response + p, TAG_ARQ_CMD_DROPPED, 0);
    }

    // 对端存活：协议连接状态；往返时延是协议层的平滑估计（微秒）
    p += tlv_u8(m_response + p, TAG_PEER_ONLINE, link.connected() ? 1 : 2);
    p += tlv_u32(m_response + p, TAG_PING_RTT, protocol.rtt_ms * 1000);
#if CONFIG_MEGUMINLINK_WIRELESS_LOSS_SIM_ENABLE
    p += tlv_u32(m_response + p, TAG_LOSS_FRAMES, stats.loss_frames);
    p += tlv_u32(m_response + p, TAG_LOSS_DROPPED, stats.loss_dropped);
#endif
    p += tlv_u32(m_response + p, TAG_UART_RX, stats.uart_rx_bytes);
    p += tlv_u32(m_response + p, TAG_UART_TX, stats.uart_tx_bytes);
    p += tlv_u32(m_response + p, TAG_UART_WIRELESS_RX, stats.uart_wireless_rx_bytes);
    p += tlv_u32(m_response + p, TAG_UART_WIRELESS_TX, stats.uart_wireless_tx_bytes);

    // 串口是否被主机打开：实时查 DTR（总线未就绪时为 false），不走缓存
#if CONFIG_MEGUMINLINK_USB_UART_BRIDGE_ENABLE
    const u8 uart_host_open = Application::get_instance().usb().cdc().host_open() ? 1 : 0;
#else
    const u8 uart_host_open = 0; // 没有 CDC 接口：不存在主机串口
#endif
    p += tlv_u8(m_response + p, TAG_UART_HOST_OPEN, uart_host_open);
    p += tlv_u32(m_response + p, TAG_UART_BAUD, stats.uart_baud);
    p += tlv_u8(m_response + p, TAG_UART_DATA_BITS, stats.uart_data_bits);
    p += tlv_u8(m_response + p, TAG_UART_STOP_BITS, stats.uart_stop_bits);
    p += tlv_u8(m_response + p, TAG_UART_PARITY, stats.uart_parity);
    p += tlv_u8(m_response + p, TAG_UART_RELIABLE, link.uart_reliable() ? 1 : 0);
    p += tlv_u32(m_response + p, TAG_UPTIME, static_cast<u32>(esp_timer_get_time() / 1000));

    return finish_response(cmd, seq, p);
}

u16 ConfigChannel::build_config_response(u8 cmd, u8 seq)
{
    const esp_app_desc_t &desc = *esp_app_get_description();
    usize p = FRAME_HEADER_SIZE;

    p += tlv_str(m_response + p, TAG_FW_VERSION, desc.version);
    p += tlv_str(m_response + p, TAG_CHIP_TARGET, CONFIG_IDF_TARGET);
    p += tlv_str(m_response + p, TAG_IDF_VERSION, desc.idf_ver);
    p += tlv_u8(m_response + p, TAG_CFG_MODE, Application::get_instance().mode() == Mode::Local ? 0 : 1);

    const u8 *peer_mac = peer_mac_key().cached();
    if (peer_mac != nullptr)
    {
        p += tlv_blob(m_response + p, TAG_CFG_PEER_MAC, peer_mac, ESP_NOW_ETH_ALEN);
    }

    p += tlv_u8(m_response + p, TAG_CFG_WIFI_RATE, Application::get_instance().wireless_link().wifi_rate());
#if CONFIG_MEGUMINLINK_WIRELESS_LOSS_SIM_ENABLE
    p += tlv_u8(m_response + p, TAG_CFG_LOSS_PERMILLE, Application::get_instance().wireless_link().loss_permille());
#endif
    {
        const meaproto::Config &config = Application::get_instance().wireless_link().proto_config();
        p += tlv_u16(m_response + p, TAG_CFG_ARQ_RTO_MIN, config.rto_initial_ms);
        p += tlv_u16(m_response + p, TAG_CFG_ARQ_RTO_INIT, config.rto_initial_ms);
        p += tlv_u16(m_response + p, TAG_CFG_ARQ_RTO_MAX, config.rto_max_ms);
        p += tlv_u16(m_response + p, TAG_CFG_ARQ_DELAYED_ACK, config.ack_delay_ms);
        p += tlv_u16(m_response + p, TAG_CFG_ARQ_PROBE, config.keepalive_interval_ms);
    }

    return finish_response(cmd, seq, p);
}

u16 ConfigChannel::build_usb_config_response(u8 cmd, u8 seq)
{
    const UsbCapabilitySettings &active = Application::get_instance().usb().usb_capability();
    usize p = FRAME_HEADER_SIZE;

    p += tlv_u8(m_response + p, TAG_USB_SUPPORTED, usb_capability_supported_mask());
    p += tlv_u8(m_response + p, TAG_USB_WEBUSB, active.webusb);
    p += tlv_u8(m_response + p, TAG_USB_WINUSB, active.winusb);
    p += tlv_u8(m_response + p, TAG_USB_HID_COMPAT, active.hid_compat);
    p += tlv_str(m_response + p, TAG_USB_LANDING_PAGE, active.landing_page);
    p += tlv_str(m_response + p, TAG_USB_DEVICE_NAME, active.device_name);
    p += tlv_u8(m_response + p, TAG_USB_DRAFT, usb_draft_state());

    const UsbConfigReject reason = usb_draft_reject();
    if (reason != UsbConfigReject::None)
    {
        p += tlv_u8(m_response + p, TAG_USB_DRAFT_REASON, static_cast<u8>(reason));
    }

    return finish_response(cmd, seq, p);
}

u8 ConfigChannel::usb_draft_state() const
{
    portENTER_CRITICAL(&m_usb_draft_lock);
    const bool present = m_usb_draft_present;
    const bool rejected = m_usb_draft_rejected;
    portEXIT_CRITICAL(&m_usb_draft_lock);

    if (!present)
    {
        return 0;
    }
    return rejected ? 2 : 1;
}

UsbConfigReject ConfigChannel::usb_draft_reject() const
{
    portENTER_CRITICAL(&m_usb_draft_lock);
    const UsbConfigReject reason = m_usb_draft_rejected ? m_usb_draft_reject : UsbConfigReject::None;
    portEXIT_CRITICAL(&m_usb_draft_lock);
    return reason;
}

bool ConfigChannel::has_valid_usb_draft() const
{
    portENTER_CRITICAL(&m_usb_draft_lock);
    const bool valid = m_usb_draft_present && !m_usb_draft_rejected;
    portEXIT_CRITICAL(&m_usb_draft_lock);
    return valid;
}

bool ConfigChannel::take_usb_config_draft(UsbCapabilitySettings *settings)
{
    portENTER_CRITICAL(&m_usb_draft_lock);
    const bool valid = m_usb_draft_present && !m_usb_draft_rejected;
    if (valid && settings != nullptr)
    {
        *settings = m_usb_draft;
    }
    m_usb_draft_present = false;
    m_usb_draft_rejected = false;
    m_usb_draft_reject = UsbConfigReject::None;
    portEXIT_CRITICAL(&m_usb_draft_lock);

    return valid;
}

namespace
{
/**
 * SetLinkConfig OUT 载荷的 tag。载荷自成一套 TLV 空间，与响应帧的 tag 无关；
 * 只列出的字段被覆盖，没列出的保持本机当前值，因此主机可以只改一项。
 *
 * 只接受运行期可改的时序类字段；尺寸类（窗口、重传次数、熔断）在构造时按存储
 * 固定，对应 tag 一律按非法处理。
 */
enum : u8
{
    // 0x01-0x05 未分配（窗口、重传次数、熔断与 RTO 下限没有运行期语义）
    LINK_TAG_RTO_INIT = 0x06,    // u16：初始 RTO（毫秒）
    LINK_TAG_RTO_MAX = 0x07,     // u16：RTO 上限（毫秒）
    LINK_TAG_DELAYED_ACK = 0x08, // u16：延迟确认（毫秒）
    LINK_TAG_PROBE = 0x09,       // u16：保活间隔（毫秒）
};

u16 read_u16_be(const u8 *value)
{
    return static_cast<u16>((static_cast<u16>(value[0]) << 8) | value[1]);
}

/**
 * 解析 SetLinkConfig 载荷，把出现的字段覆盖到 io 指向的参数上。
 * @return true=格式合法；false=TLV 越界或取值长度不对（调用方在 DATA 阶段返回
 *         false，由 TinyUSB 在状态阶段 STALL）
 */
bool parse_link_config(const u8 *data, usize len, meaproto::Config *io)
{
    usize pos = 0;
    while (pos + 2 <= len)
    {
        const u8 tag = data[pos];
        usize value_len = data[pos + 1];
        pos += 2;
        if (value_len == 0xFF)
        {
            if (pos + 2 > len)
            {
                return false;
            }
            value_len = (static_cast<usize>(data[pos]) << 8) | data[pos + 1];
            pos += 2;
        }
        if (pos + value_len > len)
        {
            return false;
        }

        const u8 *value = data + pos;
        pos += value_len;

        if (value_len != 2)
        {
            return false;
        }
        const u16 parsed = read_u16_be(value);
        switch (tag)
        {
        case LINK_TAG_RTO_INIT:
            io->rto_initial_ms = parsed;
            break;
        case LINK_TAG_RTO_MAX:
            io->rto_max_ms = parsed;
            break;
        case LINK_TAG_DELAYED_ACK:
            io->ack_delay_ms = parsed;
            break;
        case LINK_TAG_PROBE:
            io->keepalive_interval_ms = parsed;
            break;
        default:
            return false; // 未知或已失效的 tag：宁可拒掉也不猜着执行
        }
    }
    return true;
}
} // namespace

bool ConfigChannel::stage_link_config_draft(const u8 *data, usize len)
{
    // 以本机当前参数为底：载荷只覆盖出现的字段
    meaproto::Config draft = Application::get_instance().wireless_link().proto_config();
    if (!parse_link_config(data, len, &draft))
    {
        return false;
    }

    // 与 Core::apply_config 同一套拒绝条件：非法值在 DATA 阶段就 STALL
    if (draft.rto_initial_ms == 0 || draft.rto_max_ms < draft.rto_initial_ms || draft.connect_timeout_ms == 0 ||
        draft.keepalive_interval_ms == 0 || draft.send_quota_per_tick == 0)
    {
        return false;
    }

    portENTER_CRITICAL(&m_link_draft_lock);
    m_link_draft = draft;
    m_link_draft_present = true;
    portEXIT_CRITICAL(&m_link_draft_lock);

    // 参数下发即应用，因此这里直接投递动作；槽被占用时按 STALL 让主机重试
    return post_action(Action::SetLinkConfig);
}

bool ConfigChannel::handle_link_config_out(u8 rhport, u8 stage, const tusb_control_request_t *request)
{
    if (static_cast<u8>(request->wValue & 0xFF) != static_cast<u8>(Cmd::SetLinkConfig))
    {
        return false; // 其余 OUT 厂商请求不属于本通道
    }

    if (stage == CONTROL_STAGE_SETUP)
    {
        if (request->wLength == 0 || request->wLength > sizeof(m_link_draft_buffer))
        {
            return false; // STALL
        }
        tud_control_xfer(rhport, request, m_link_draft_buffer, request->wLength);
        return true;
    }

    if (stage == CONTROL_STAGE_DATA)
    {
        // 格式非法时返回 false：TinyUSB 在状态阶段 STALL，主机端直接拿到失败
        return stage_link_config_draft(m_link_draft_buffer, request->wLength);
    }

    return true; // ACK 阶段：载荷已在 DATA 阶段处理完
}

bool ConfigChannel::stage_usb_config_draft(const u8 *data, usize len)
{
    ParsedUsbConfig parsed;
    const UsbCapabilitySettings &active = Application::get_instance().usb().usb_capability();
    if (!parse_usb_config(data, len, active, &parsed))
    {
        return false; // 格式非法：不改草稿，交回调用方在状态阶段 STALL
    }

    UsbCapabilitySettings draft = parsed.settings;
    if (draft.landing_page[0] == '\0')
    {
        // 空 URL = 回退编译期默认值（"不设 landing page"与"关闭 WebUSB"是两件事）
        detail::copy_cstr(draft.landing_page, sizeof(draft.landing_page), usb_landing_page_default());
    }

    // 超长按原始长度判定：草稿里只留得下截断值，不能拿它比较
    UsbConfigReject reject = UsbConfigReject::None;
    if (parsed.landing_page_len > USB_LANDING_PAGE_MAX_LEN || parsed.device_name_len > USB_DEVICE_NAME_MAX_LEN)
    {
        reject = UsbConfigReject::TooLong;
    }
    else
    {
        reject = usb_capability_check(draft);
    }

    portENTER_CRITICAL(&m_usb_draft_lock);
    m_usb_draft = draft;
    m_usb_draft_present = true;
    m_usb_draft_rejected = reject != UsbConfigReject::None;
    m_usb_draft_reject = reject;
    portEXIT_CRITICAL(&m_usb_draft_lock);

    return true;
}

bool ConfigChannel::handle_usb_config_out(u8 rhport, u8 stage, const tusb_control_request_t *request)
{
    if (static_cast<u8>(request->wValue & 0xFF) != static_cast<u8>(Cmd::SetUsbConfig))
    {
        return false; // 其余 OUT 厂商请求不属于本通道
    }

    if (stage == CONTROL_STAGE_SETUP)
    {
        // 编译期两个能力都未启用时没有可调项；空载荷与超长载荷都是协议级错误
        if (usb_capability_supported_mask() == 0 || request->wLength == 0 ||
            request->wLength > sizeof(m_usb_draft_buffer))
        {
            return false; // STALL
        }
        tud_control_xfer(rhport, request, m_usb_draft_buffer, request->wLength);
        return true;
    }

    if (stage == CONTROL_STAGE_DATA)
    {
        // 格式非法时返回 false：TinyUSB 在状态阶段 STALL 两个方向的控制端点，
        // 主机端的 controlTransferOut 直接得到失败
        return stage_usb_config_draft(m_usb_draft_buffer, request->wLength);
    }

    return true; // ACK 阶段：载荷已在 DATA 阶段处理完
}

bool ConfigChannel::handle_control(u8 rhport, u8 stage, const tusb_control_request_t *request)
{
    if (request->bRequest != VENDOR_CODE)
    {
        return false;
    }

    // OUT 命令走独立分支：SETUP 声明接收、DATA 收载荷，没有 DATA IN 阶段，因此
    // 各自那套阶段处理在自己的 handle_*_out 里。两个命令都不认才交回调用方。
    if (request->bmRequestType_bit.direction != TUSB_DIR_IN)
    {
        if (handle_usb_config_out(rhport, stage, request))
        {
            return true;
        }
        return handle_link_config_out(rhport, stage, request);
    }

    if (stage != CONTROL_STAGE_SETUP)
    {
        // 已接管的请求：其余阶段由协议栈继续处理，此处无额外动作
        return true;
    }

    const u8 cmd = static_cast<u8>(request->wValue & 0xFF);
    const u8 arg = static_cast<u8>(request->wValue >> 8);
    const u8 seq = static_cast<u8>(request->wIndex & 0xFF);

    // 需要两端一致的命令先转发给对端，再走本机：转发帧因此走当前（仍然可用的）
    // 链路参数，不会出现"本机已切到问题档位、转发帧随即丢失"的两端不一致
    if (cmd == static_cast<u8>(Cmd::SetWifiRate) && mlink::wifi_rate_supported(arg))
    {
        relay_to_peer(cmd, arg);
    }
#if CONFIG_MEGUMINLINK_WIRELESS_LOSS_SIM_ENABLE
    // 丢包注入只在发送端生效：两端必须各自持有同一个值，只改一端只降单方向的
    // 链路质量。转发帧走 CONFIG 通道，不在注入白名单里，丢包率开到上限也改得回来。
    else if (cmd == static_cast<u8>(Cmd::SetLoss) && arg <= mlink::MAX_LOSS_PERMILLE)
    {
        relay_to_peer(cmd, arg);
    }
#endif
    // 串口出向的可靠性是两端各自的选择，但两端必须一致：只改一端就会出现"下载走
    // 可靠、回包走尽力而为"的半可靠链路，吞吐与丢帧现象无法归因
    else if (cmd == static_cast<u8>(Cmd::SetUartReliable) && arg <= 1)
    {
        relay_to_peer(cmd, arg);
    }

    u16 len = dispatch_control(cmd, arg, seq);
    if (len > request->wLength)
    {
        len = static_cast<u16>(request->wLength); // 主机要得更少：按 wLength 截断
    }
    tud_control_xfer(rhport, request, m_response, len);
    return true;
}

void ConfigChannel::relay_to_peer(u8 cmd, u8 arg)
{
    // 原样复刻主机那条控制请求：命令码与参数在 wValue、seq 在 wIndex，没有载荷
    payload::RelayedUsbControl relay = {};
    relay.len = 0;
    relay.bm_request_type = CONTROL_DIR_IN;
    relay.b_request = VENDOR_CODE;
    relay.w_value = static_cast<u16>(cmd) | (static_cast<u16>(arg) << 8);
    relay.w_index = 0;
    relay.w_length = 0;
    relay.data = nullptr;

    const meaproto::SendResult result = Application::get_instance().wireless_link().send_relayed_usb_control(relay);
    if (result != meaproto::SendResult::Ok)
    {
        ESP_LOGW(TAG, "Failed to relay command 0x%02X (result %u)", cmd, static_cast<unsigned>(result));
    }
}

#endif // CONFIG_MEGUMINLINK_USB_SUPPORT

bool ConfigChannel::take_link_config_draft(meaproto::Config *config)
{
    portENTER_CRITICAL(&m_link_draft_lock);
    const bool present = m_link_draft_present;
    if (present && config != nullptr)
    {
        *config = m_link_draft;
    }
    m_link_draft_present = false;
    portEXIT_CRITICAL(&m_link_draft_lock);
    return present;
}
