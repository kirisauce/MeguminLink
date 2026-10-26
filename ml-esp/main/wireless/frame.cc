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
    // 各分支只算"类型 + 帧体"的长度
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
    case PacketType::RelayedUsbControl: {
        // 类型 1B + len 2B + SETUP 包 8B + 载荷
        body = 1 + 2 + payload::RELAYED_CONTROL_HEADER_SIZE + payload.relayed_usb_control.len;
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
    case PacketType::RelayedUsbControl: {
        auto &content = payload.relayed_usb_control;
        if (content.len > payload::MAX_RELAYED_CONTROL_PAYLOAD || (content.len > 0 && content.data == nullptr))
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
