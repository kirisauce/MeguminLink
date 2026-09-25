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
    // 各分支只算"类型 + 帧体"的长度：业务包不含 ARQ 头，那一层由 arq_link 封装
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
    case PacketType::DapCommand: {
        // 命令帧体比响应多一个通道标记字节（见 DAP_CHANNEL_FIELD_SIZE）
        body = DAP_FRAME_HEADER_SIZE + DAP_CHANNEL_FIELD_SIZE + payload.dap_command.len;
        break;
    }
    case PacketType::DapResponse: {
        body = DAP_FRAME_HEADER_SIZE + payload.dap_response.len;
        break;
    }
    case PacketType::UartFromHost:
    case PacketType::UartFromTarget: {
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
    default: {
        // 与 serialize() 的 ESP_ERR_NOT_SUPPORTED 对齐：未知类型按非法处理
        return 0;
    }
    }

    return body;
}

esp_err_t Packet::deserialize(const u8 *data, size_t len, u8 *storage, size_t storage_cap)
{
    BinDeserializer p(data, data + len);

    MLINK_CHECK_SIZE(p, 1);
    packet_type = static_cast<PacketType>(p.read_u8());

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
        MLINK_CHECK_SIZE(p, 3);
        const u16 n = p.read_u16();
        const u8 channel = p.read_u8();
        if (n > storage_cap)
        {
            return ESP_ERR_INVALID_SIZE;
        }
        MLINK_CHECK_SIZE(p, n);
        p.read_bytes(storage, n);
        content.len = n;
        content.channel = channel;
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
    case PacketType::UartFromHost:
    case PacketType::UartFromTarget: {
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
        MLINK_CHECK_SIZE(s, 3);
        s.write_u16(content.len);
        s.write_u8(content.channel);
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
    case PacketType::UartFromHost:
    case PacketType::UartFromTarget: {
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
    default: {
        return ESP_ERR_NOT_SUPPORTED;
    }
    }

    return ESP_OK;
}

#undef MLINK_CHECK_SIZE
