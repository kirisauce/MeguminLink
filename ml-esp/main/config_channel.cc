#include "meguminlink/config_channel.hpp"

#include "meguminlink/app.hpp"
#include "meguminlink/nvs_flash.hpp" // peer_mac_load + esp_now.h（ESP_NOW_ETH_ALEN）

#include <esp_app_desc.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <sdkconfig.h>

#include <cstring>

using namespace mlink;

void ConfigChannel::add_dap_command(u16 request_bytes, u16 response_bytes)
{
    m_dap_cmds = m_dap_cmds + 1;
    m_dap_request_bytes = m_dap_request_bytes + request_bytes;
    m_dap_response_bytes = m_dap_response_bytes + response_bytes;
}

void ConfigChannel::add_wireless_tx(u16 bytes)
{
    m_wireless_tx_bytes = m_wireless_tx_bytes + bytes;
}

void ConfigChannel::add_wireless_rx(u16 bytes, i8 rssi)
{
    m_wireless_rx_bytes = m_wireless_rx_bytes + bytes;
    m_wireless_rssi = rssi;
}

void ConfigChannel::add_uart_rx(u16 bytes)
{
    m_uart_rx_bytes = m_uart_rx_bytes + bytes;
}

void ConfigChannel::add_uart_tx(u16 bytes)
{
    m_uart_tx_bytes = m_uart_tx_bytes + bytes;
}

void ConfigChannel::add_uart_wireless_rx(u16 bytes)
{
    m_uart_wireless_rx_bytes = m_uart_wireless_rx_bytes + bytes;
}

void ConfigChannel::add_uart_wireless_tx(u16 bytes)
{
    m_uart_wireless_tx_bytes = m_uart_wireless_tx_bytes + bytes;
}

#if CONFIG_MEGUMINLINK_USB_SUPPORT

static constexpr char TAG[] = "mlink::config_channel";

// ---- 协议（固件与上位机的唯一契约，上位机见 tools/mlinkctl.py 与
//      tools/mlink-webui/src/lib/protocol.ts）----
//
// 传输：EP0 厂商控制请求，一律 IN 方向（bmRequestType = 0xC0：IN、vendor、
// device recipient）、bRequest = 0x4D（'M'）、wValue = 命令码 | (参数 << 8)、
// wIndex = seq（原样回显）、wLength = 期望响应长度。请求没有 DATA OUT 阶段，
// 命令参数因此走 wValue；响应帧在 DATA IN 阶段回给主机，一次往返拿到结果。
// OUT 方向的厂商请求不属于本通道，交回调用方 STALL。
//
// 帧头 8B（大端）：magic u16（0x4D4C，字节流恒为 'M' 'L'）+ cmd u8 + seq u8
// + len u16（TLV 区长度）+ status u8 + reserved u8，后接 len 字节 TLV 区。
// TLV：tag u8 | len u8 | value，len == 0xFF 表示扩展长度（后接 u16 大端）；
// 整数一律大端，字符串不带结尾 NUL。新增 tag 不影响旧上位机解析。
//
// 命令表（0x01-0x0F 只读，0x10-0x1F 动作）：
//   0x01 GetStats     读统计快照
//   0x02 GetConfig    读设备身份与配置项
//   0x10 SetMode      参数 0=Local 1=Wireless，其他值回 malformed
//   0x11 EnterPairing 进入配对流程（无线模式下有效，Local 记日志忽略）
//   0x12 ClearPair    清除配对记录并进入配对流程
//   0x13 Reset        清除配对与模式记录后重启
//   0x14 Reboot       立即重启
//   0x15 ExitPairing  退出配对流程（无线模式且配对进行中有效）
// 动作命令的响应 TLV 区为空：status == ok 表示动作已投递（执行在主循环，
// 完成与否看串口日志与设备行为），busy 表示槽内已有未取走的动作。
namespace
{
constexpr u8 VENDOR_CODE = 0x4D;
constexpr u16 FRAME_MAGIC = 0x4D4C;
constexpr usize FRAME_HEADER_SIZE = 8;

enum class Cmd : u8
{
    GetStats = 0x01,
    GetConfig = 0x02,
    SetMode = 0x10,
    EnterPairing = 0x11,
    ClearPair = 0x12,
    Reset = 0x13,
    Reboot = 0x14,
    ExitPairing = 0x15,
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

// tag 段位：0x01-0x0F 设备状态、0x10-0x1F DAP、0x20-0x2F 无线、0x30-0x3F UART、
// 0x40-0x4F 系统、0x50-0x5F 配置项。
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
    TAG_UART_RX = 0x30,          // u32：Local 模式 CDC → UART 字节数
    TAG_UART_TX = 0x31,          // u32：Local 模式 UART → CDC 字节数
    TAG_UART_WIRELESS_RX = 0x32, // u32：Wireless 模式 CDC → 无线 字节数
    TAG_UART_WIRELESS_TX = 0x33, // u32：Wireless 模式 无线 → CDC 字节数
    TAG_UPTIME = 0x40,           // u32：上电毫秒数（49.7 天回绕）
    TAG_FW_VERSION = 0x41,       // string：应用版本（esp_app_desc.version）
    TAG_CHIP_TARGET = 0x42,      // string：目标芯片（CONFIG_IDF_TARGET）
    TAG_IDF_VERSION = 0x43,      // string：ESP-IDF 版本（esp_app_desc.idf_ver）
    TAG_CFG_MODE = 0x50,         // u8：持久化的运行模式 0=Local 1=Wireless
    TAG_CFG_PEER_MAC = 0x51,     // blob 6B：已配对对端 MAC（未配对时不下发）
};

usize tlv_u8(u8 *out, u8 tag, u8 value)
{
    out[0] = tag;
    out[1] = 1;
    out[2] = value;
    return 3;
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
    u8 peer_mac[ESP_NOW_ETH_ALEN];
    return peer_mac_load(peer_mac) ? 1 : 0;
}
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
    usize p = FRAME_HEADER_SIZE;

    p += tlv_u8(m_response + p, TAG_MODE, Application::get_instance().mode() == Mode::Local ? 0 : 1);
    p += tlv_u8(m_response + p, TAG_PAIR, pair_state());
    p += tlv_i8(m_response + p, TAG_RSSI, m_wireless_rssi);
    p += tlv_u32(m_response + p, TAG_DAP_CMDS, m_dap_cmds);
    p += tlv_u32(m_response + p, TAG_DAP_REQUEST, m_dap_request_bytes);
    p += tlv_u32(m_response + p, TAG_DAP_RESPONSE, m_dap_response_bytes);
    p += tlv_u32(m_response + p, TAG_WIRELESS_TX, m_wireless_tx_bytes);
    p += tlv_u32(m_response + p, TAG_WIRELESS_RX, m_wireless_rx_bytes);
    p += tlv_u32(m_response + p, TAG_UART_RX, m_uart_rx_bytes);
    p += tlv_u32(m_response + p, TAG_UART_TX, m_uart_tx_bytes);
    p += tlv_u32(m_response + p, TAG_UART_WIRELESS_RX, m_uart_wireless_rx_bytes);
    p += tlv_u32(m_response + p, TAG_UART_WIRELESS_TX, m_uart_wireless_tx_bytes);
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

    u8 peer_mac[ESP_NOW_ETH_ALEN];
    if (peer_mac_load(peer_mac))
    {
        p += tlv_blob(m_response + p, TAG_CFG_PEER_MAC, peer_mac, sizeof(peer_mac));
    }

    return finish_response(cmd, seq, p);
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

bool ConfigChannel::handle_control(u8 rhport, u8 stage, const tusb_control_request_t *request)
{
    if (request->bRequest != VENDOR_CODE)
    {
        return false;
    }
    if (stage != CONTROL_STAGE_SETUP)
    {
        // 已接管的请求：其余阶段由协议栈继续处理，此处无额外动作
        return true;
    }

    // 本通道一律 IN 请求（命令码与参数都在 SETUP 包的 wValue 里）：OUT 方向
    // 的厂商请求不属于本通道，未识别的方向直接 STALL
    if (request->bmRequestType_bit.direction != TUSB_DIR_IN)
    {
        return false;
    }

    const u8 cmd = static_cast<u8>(request->wValue & 0xFF);
    const u8 arg = static_cast<u8>(request->wValue >> 8);
    const u8 seq = static_cast<u8>(request->wIndex & 0xFF);
    u16 len = FRAME_HEADER_SIZE;
    switch (static_cast<Cmd>(cmd))
    {
    case Cmd::GetStats:
        len = build_stats_response(cmd, seq);
        break;
    case Cmd::GetConfig:
        len = build_config_response(cmd, seq);
        break;
    case Cmd::SetMode:
        // 参数只有 local/wireless 两个取值，其余按 malformed 拒绝且不占动作槽
        len = arg <= static_cast<u8>(Mode::Wireless)
                  ? post_action_response(cmd, seq, Action::SetMode, arg)
                  : build_empty_response(cmd, seq, static_cast<u8>(Status::Malformed));
        break;
    case Cmd::EnterPairing:
        len = post_action_response(cmd, seq, Action::EnterPairing, 0);
        break;
    case Cmd::ClearPair:
        len = post_action_response(cmd, seq, Action::ClearPair, 0);
        break;
    case Cmd::Reset:
        len = post_action_response(cmd, seq, Action::Reset, 0);
        break;
    case Cmd::Reboot:
        len = post_action_response(cmd, seq, Action::Reboot, 0);
        break;
    case Cmd::ExitPairing:
        len = post_action_response(cmd, seq, Action::ExitPairing, 0);
        break;
    default:
        // 未定义的命令码：回带 UnknownCommand 状态的空帧（不回 STALL，
        // 让上位机能明确区分"设备不支持该命令"与"传输层失败"）
        len = build_empty_response(cmd, seq, static_cast<u8>(Status::UnknownCommand));
        break;
    }

    if (len > request->wLength)
    {
        len = static_cast<u16>(request->wLength); // 主机要得更少：按 wLength 截断
    }
    tud_control_xfer(rhport, request, m_response, len);
    return true;
}

#endif // CONFIG_MEGUMINLINK_USB_SUPPORT
