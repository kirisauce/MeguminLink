#include "meguminlink/wireless/frame.hpp"
#include "meguminlink/binfmt.hpp"

using namespace mlink;

// ---- Packet 序列化 ----

#define MLINK_CHECK_SIZE(parser, expected_size)                                                                        \
    if (parser.remaining() < expected_size)                                                                            \
    {                                                                                                                  \
        return ESP_ERR_INVALID_SIZE;                                                                                   \
    }

Packet::Packet()
{
}

size_t Packet::serialized_size() const
{
    // 各分支只算"类型 + 帧体"的长度，扩展头长度在末尾统一补上：新增帧体字段时
    // 只需改一处，不会漏加导致发送被截断
    size_t body = 0;
    switch (packet_type)
    {
    case PacketType::Probe: {
        body = 1;
        break;
    }
    case PacketType::PairingRequest: {
        body = 1 + ESP_NOW_ETH_ALEN;
        break;
    }
    case PacketType::PairingAccepted: {
        body = 1 + ESP_NOW_ETH_ALEN;
        break;
    }
    case PacketType::DapCommand:
    case PacketType::DapResponse: {
        const u16 len = (packet_type == PacketType::DapCommand) ? payload.dap_command.len : payload.dap_response.len;
        body = DAP_FRAME_HEADER_SIZE + len;
        break;
    }
    case PacketType::UartData: {
        body = UART_FRAME_HEADER_SIZE + payload.uart_data.len;
        break;
    }
    case PacketType::UartLineCoding: {
        // 帧头 3B + 数据段 7B（baud 4B + 3 个 1B）
        body = UART_FRAME_HEADER_SIZE + 7;
        break;
    }
    case PacketType::Ping:
    case PacketType::Pong: {
        // 帧头 3B + 数据段 4B（token）
        body = UART_FRAME_HEADER_SIZE + 4;
        break;
    }
    case PacketType::RelayedUsbControl: {
        // 帧头 3B + len 2B + SETUP 包 8B + 载荷
        body = UART_FRAME_HEADER_SIZE + 2 + payload::RELAYED_CONTROL_HEADER_SIZE + payload.relayed_usb_control.len;
        break;
    }
#if CONFIG_MEGUMINLINK_WIRELESS_ARQ_ENABLE
    case PacketType::Ack: {
        body = 1; // 只有类型字节：纯确认帧没有帧体，信息全在扩展头里
        break;
    }
    case PacketType::ArqReset: {
        // 类型 1B + 数据段；与 Probe 一样不带 len 字段
        body = 1 + ARQ_RESET_PAYLOAD_SIZE;
        break;
    }
#endif
    default: {
        // 与 serialize() 的 ESP_ERR_NOT_SUPPORTED 对齐：未知类型按非法处理
        return 0;
    }
    }

    return body + ARQ_HEADER_SIZE;
}

esp_err_t Packet::deserialize(const u8 *data, size_t len, u8 *storage, size_t storage_cap)
{
    BinDeserializer p(data, data + len);

    MLINK_CHECK_SIZE(p, 1);
    packet_type = static_cast<PacketType>(p.read_u8());

#if CONFIG_MEGUMINLINK_WIRELESS_ARQ_ENABLE
    // 扩展头位置固定（紧跟在类型字节之后）、与帧类型无关，因此在 switch 之前
    // 统一读一次，各分支都不需要感知它
    MLINK_CHECK_SIZE(p, ARQ_HEADER_SIZE);
    arq.seq = p.read_u16();
    arq.ack = p.read_u16();
    arq.window = p.read_u8();
    arq.flags = p.read_u8();
    arq.sack = p.read_u16();
#endif

    switch (packet_type)
    {
    case PacketType::Probe: {
        break;
    }
    case PacketType::PairingRequest: {
        auto &content = payload.pairing_request;
        MLINK_CHECK_SIZE(p, 6);
        p.read_bytes(content.target_mac, sizeof(content.target_mac));
        break;
    }
    case PacketType::PairingAccepted: {
        auto &content = payload.pairing_accepted;
        MLINK_CHECK_SIZE(p, 6);
        p.read_bytes(content.target_mac, sizeof(content.target_mac));
        break;
    }
    case PacketType::DapCommand: {
        // 数据段拷入调用方存储，data 指向该存储（见 ReceivedPacket::storage）
        auto &content = payload.dap_command;
        MLINK_CHECK_SIZE(p, 2);
        const u16 n = p.read_u16();
        if (n > storage_cap)
        {
            return ESP_ERR_INVALID_SIZE;
        }
        MLINK_CHECK_SIZE(p, n);
        p.read_bytes(storage, n);
        content.len = n;
        content.data = storage;
        break;
    }
    case PacketType::DapResponse: {
        auto &content = payload.dap_response;
        MLINK_CHECK_SIZE(p, 2);
        const u16 n = p.read_u16();
        if (n > storage_cap)
        {
            return ESP_ERR_INVALID_SIZE;
        }
        MLINK_CHECK_SIZE(p, n);
        p.read_bytes(storage, n);
        content.len = n;
        content.data = storage;
        break;
    }
    case PacketType::UartData: {
        auto &content = payload.uart_data;
        MLINK_CHECK_SIZE(p, 2);
        const u16 n = p.read_u16();
        if (n > storage_cap)
        {
            return ESP_ERR_INVALID_SIZE;
        }
        MLINK_CHECK_SIZE(p, n);
        p.read_bytes(storage, n);
        content.len = n;
        content.data = storage;
        break;
    }
    case PacketType::UartLineCoding: {
        // 数据段固定 7B：baud 4B + data_bits/stop_bits/parity 各 1B
        auto &content = payload.uart_line_coding;
        MLINK_CHECK_SIZE(p, 2);
        (void)p.read_u16(); // len（防御性读取并忽略，固定 7B）
        MLINK_CHECK_SIZE(p, 7);
        content.baud = p.read_u32();
        content.data_bits = p.read_u8();
        content.stop_bits = p.read_u8();
        content.parity = p.read_u8();
        break;
    }
    case PacketType::Ping: {
        // 数据段固定 4B：token
        auto &content = payload.ping;
        MLINK_CHECK_SIZE(p, 2);
        (void)p.read_u16(); // len（防御性读取并忽略，固定 4B）
        MLINK_CHECK_SIZE(p, 4);
        content.token = p.read_u32();
        break;
    }
    case PacketType::Pong: {
        auto &content = payload.pong;
        MLINK_CHECK_SIZE(p, 2);
        (void)p.read_u16(); // len（防御性读取并忽略，固定 4B）
        MLINK_CHECK_SIZE(p, 4);
        content.token = p.read_u32();
        break;
    }
    case PacketType::RelayedUsbControl: {
        // 数据段：[len:2][SETUP 8B][载荷:len]；载荷拷入调用方存储，data 指向该存储
        auto &content = payload.relayed_usb_control;
        MLINK_CHECK_SIZE(p, 2);
        const u16 n = p.read_u16();
        if (n > storage_cap || n > payload::MAX_RELAYED_CONTROL_PAYLOAD)
        {
            return ESP_ERR_INVALID_SIZE;
        }
        MLINK_CHECK_SIZE(p, payload::RELAYED_CONTROL_HEADER_SIZE);
        content.bm_request_type = p.read_u8();
        content.b_request = p.read_u8();
        content.w_value = p.read_u16();
        content.w_index = p.read_u16();
        content.w_length = p.read_u16();
        MLINK_CHECK_SIZE(p, n);
        p.read_bytes(storage, n);
        content.len = n;
        content.data = storage;
        break;
    }
#if CONFIG_MEGUMINLINK_WIRELESS_ARQ_ENABLE
    case PacketType::Ack: {
        // 纯确认帧没有帧体：累积确认号、SACK 位图与窗口通告全在扩展头里
        break;
    }
    case PacketType::ArqReset: {
        auto &content = payload.arq_reset;
        MLINK_CHECK_SIZE(p, ARQ_RESET_PAYLOAD_SIZE);
        content.epoch = p.read_u32();
        content.settings.generation = p.read_u32();
        content.settings.enable = p.read_u8();
        content.settings.window = p.read_u8();
        content.settings.max_retx = p.read_u8();
        content.settings.breaker_failures = p.read_u8();
        content.settings.rto_min_ms = p.read_u16();
        content.settings.initial_rto_ms = p.read_u16();
        content.settings.rto_max_ms = p.read_u16();
        content.settings.delayed_ack_ms = p.read_u16();
        content.settings.breaker_probe_ms = p.read_u16();
        break;
    }
#endif
    default: {
        break;
    }
    }

    return ESP_OK;
}

esp_err_t Packet::serialize(u8 *buffer, size_t len) const
{
    BinSerializer s(buffer, len);
    MLINK_CHECK_SIZE(s, 1);
    s.write_u8(static_cast<u8>(packet_type));

#if CONFIG_MEGUMINLINK_WIRELESS_ARQ_ENABLE
    // 扩展头写在类型字节之后、帧体之前，所有帧统一携带——各分支都不需要感知它
    MLINK_CHECK_SIZE(s, ARQ_HEADER_SIZE);
    s.write_u16(arq.seq);
    s.write_u16(arq.ack);
    s.write_u8(arq.window);
    s.write_u8(arq.flags);
    s.write_u16(arq.sack);
#endif

    switch (packet_type)
    {
    case PacketType::Probe: {
        break;
    }
    case PacketType::PairingRequest: {
        auto &content = payload.pairing_request;
        MLINK_CHECK_SIZE(s, 6);
        s.write_bytes(content.target_mac, sizeof(content.target_mac));
        break;
    }
    case PacketType::PairingAccepted: {
        auto &content = payload.pairing_accepted;
        MLINK_CHECK_SIZE(s, 6);
        s.write_bytes(content.target_mac, sizeof(content.target_mac));
        break;
    }
    case PacketType::DapCommand: {
        auto &content = payload.dap_command;
        if (content.data == nullptr || content.len > MAX_DAP_PAYLOAD_SIZE)
        {
            return ESP_ERR_INVALID_SIZE;
        }
        MLINK_CHECK_SIZE(s, 2);
        s.write_u16(content.len);
        MLINK_CHECK_SIZE(s, content.len);
        s.write_bytes(content.data, content.len);
        break;
    }
    case PacketType::DapResponse: {
        auto &content = payload.dap_response;
        if (content.data == nullptr || content.len > MAX_DAP_PAYLOAD_SIZE)
        {
            return ESP_ERR_INVALID_SIZE;
        }
        MLINK_CHECK_SIZE(s, 2);
        s.write_u16(content.len);
        MLINK_CHECK_SIZE(s, content.len);
        s.write_bytes(content.data, content.len);
        break;
    }
    case PacketType::UartData: {
        auto &content = payload.uart_data;
        if (content.data == nullptr || content.len > MAX_UART_PAYLOAD_SIZE)
        {
            return ESP_ERR_INVALID_SIZE;
        }
        MLINK_CHECK_SIZE(s, 2);
        s.write_u16(content.len);
        MLINK_CHECK_SIZE(s, content.len);
        s.write_bytes(content.data, content.len);
        break;
    }
    case PacketType::UartLineCoding: {
        auto &content = payload.uart_line_coding;
        MLINK_CHECK_SIZE(s, 2 + 7);
        s.write_u16(7);
        s.write_u32(content.baud);
        s.write_u8(content.data_bits);
        s.write_u8(content.stop_bits);
        s.write_u8(content.parity);
        break;
    }
    case PacketType::Ping: {
        auto &content = payload.ping;
        MLINK_CHECK_SIZE(s, 2 + 4);
        s.write_u16(4);
        s.write_u32(content.token);
        break;
    }
    case PacketType::Pong: {
        auto &content = payload.pong;
        MLINK_CHECK_SIZE(s, 2 + 4);
        s.write_u16(4);
        s.write_u32(content.token);
        break;
    }
    case PacketType::RelayedUsbControl: {
        auto &content = payload.relayed_usb_control;
        if (content.len > payload::MAX_RELAYED_CONTROL_PAYLOAD ||
            (content.len > 0 && content.data == nullptr))
        {
            return ESP_ERR_INVALID_SIZE;
        }
        MLINK_CHECK_SIZE(s, 2 + payload::RELAYED_CONTROL_HEADER_SIZE);
        s.write_u16(content.len);
        s.write_u8(content.bm_request_type);
        s.write_u8(content.b_request);
        s.write_u16(content.w_value);
        s.write_u16(content.w_index);
        s.write_u16(content.w_length);
        MLINK_CHECK_SIZE(s, content.len);
        s.write_bytes(content.data, content.len);
        break;
    }
#if CONFIG_MEGUMINLINK_WIRELESS_ARQ_ENABLE
    case PacketType::Ack: {
        // 纯确认帧没有帧体：只发类型与扩展头
        break;
    }
    case PacketType::ArqReset: {
        // 会话重置帧同时携带参数：这类帧周期性重发直到握手完成，参数因此自带
        // 重传保证，不用再借那条尽力而为的中继通道
        auto &content = payload.arq_reset;
        MLINK_CHECK_SIZE(s, ARQ_RESET_PAYLOAD_SIZE);
        s.write_u32(content.epoch);
        s.write_u32(content.settings.generation);
        s.write_u8(content.settings.enable);
        s.write_u8(content.settings.window);
        s.write_u8(content.settings.max_retx);
        s.write_u8(content.settings.breaker_failures);
        s.write_u16(content.settings.rto_min_ms);
        s.write_u16(content.settings.initial_rto_ms);
        s.write_u16(content.settings.rto_max_ms);
        s.write_u16(content.settings.delayed_ack_ms);
        s.write_u16(content.settings.breaker_probe_ms);
        break;
    }
#endif
    default: {
        return ESP_ERR_NOT_SUPPORTED;
    }
    }

    return ESP_OK;
}

#undef MLINK_CHECK_SIZE
