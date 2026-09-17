#include "meguminlink/config_channel.hpp"

#include "meguminlink/app.hpp"
#include "meguminlink/nvs_flash.hpp" // peer_mac_load + esp_now.h（ESP_NOW_ETH_ALEN）

#include <esp_log.h>
#include <esp_timer.h>
#include <sdkconfig.h>

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

// ---- 协议（固件与上位机的唯一契约，上位机见 tools/mlinkctl.py）----
//
// 传输：EP0 厂商控制请求。bmRequestType = 0xC0（IN，vendor，device recipient）、
// bRequest = 0x4D（'M'）、wValue = 命令码、wIndex = seq（原样回显）、
// wLength = 期望响应长度。只读命令没有 DATA OUT 阶段，命令参数因此走
// wValue —— 请求不带帧头，响应才是完整一帧。
//
// 帧头 8B（大端）：magic u16（0x4D4C，字节流恒为 'M' 'L'）+ cmd u8 + seq u8
// + len u16（TLV 区长度）+ status u8 + reserved u8，后接 len 字节 TLV 区。
// TLV：tag u8 | len u8 | value，len == 0xFF 表示扩展长度（后接 u16 大端）；
// 整数一律大端，字符串不带结尾 NUL。新增 tag 不影响旧上位机解析。
namespace
{
constexpr u8 VENDOR_CODE = 0x4D;
constexpr u16 FRAME_MAGIC = 0x4D4C;
constexpr usize FRAME_HEADER_SIZE = 8;

enum class Cmd : u8
{
    GetStats = 0x01,
};

enum class Status : u8
{
    Ok = 0x00,
    UnknownCommand = 0x01,
    Malformed = 0x02,
    Internal = 0x03,
    Unsupported = 0x04, // 命令已定义但本固件未实现（如配置写入）
};

// tag 段位：0x01-0x0F 设备状态、0x10-0x1F DAP、0x20-0x2F 无线、0x30-0x3F UART、
// 0x40-0x4F 系统、0x50-0x5F 配置项读写（预留，第二批 WebUSB 等配置项落这里）。
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

u16 ConfigChannel::build_response(u8 cmd, u8 seq)
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

    if (p > sizeof(m_response))
    {
        // 缓冲容量不足（新增 tag 却忘了扩容量）：返回空 TLV 区而非越界
        ESP_LOGE(TAG, "Response buffer too small (%u > %u)", static_cast<unsigned>(p),
                 static_cast<unsigned>(sizeof(m_response)));
        p = FRAME_HEADER_SIZE;
    }

    const u16 payload_len = static_cast<u16>(p - FRAME_HEADER_SIZE);
    m_response[0] = static_cast<u8>(FRAME_MAGIC >> 8);
    m_response[1] = static_cast<u8>(FRAME_MAGIC);
    m_response[2] = cmd;
    m_response[3] = seq;
    m_response[4] = static_cast<u8>(payload_len >> 8);
    m_response[5] = static_cast<u8>(payload_len);
    m_response[6] = static_cast<u8>(p == FRAME_HEADER_SIZE ? Status::Internal : Status::Ok);
    m_response[7] = 0;
    return static_cast<u16>(p);
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

    // 本批只读：OUT 方向（配置写入）留到第二批，未识别的方向直接 STALL
    if (request->bmRequestType_bit.direction != TUSB_DIR_IN)
    {
        return false;
    }

    const u8 cmd = static_cast<u8>(request->wValue & 0xFF);
    const u8 seq = static_cast<u8>(request->wIndex & 0xFF);
    u16 len = FRAME_HEADER_SIZE;
    switch (static_cast<Cmd>(cmd))
    {
    case Cmd::GetStats:
        len = build_response(cmd, seq);
        break;
    default:
        // 未定义的命令码：回带 UnknownCommand 状态的空帧（不回 STALL，
        // 让上位机能明确区分"设备不支持该命令"与"传输层失败"）
        m_response[0] = static_cast<u8>(FRAME_MAGIC >> 8);
        m_response[1] = static_cast<u8>(FRAME_MAGIC);
        m_response[2] = cmd;
        m_response[3] = seq;
        m_response[4] = 0;
        m_response[5] = 0;
        m_response[6] = static_cast<u8>(Status::UnknownCommand);
        m_response[7] = 0;
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
