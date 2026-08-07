#pragma once
#ifndef __MEGUMINLINK_WIRELESS_LINK_HPP__
#define __MEGUMINLINK_WIRELESS_LINK_HPP__

#include "esp_err.h"
#include "esp_now.h"              // IWYU pragma: export
#include "esp_wifi.h"             // IWYU pragma: export
#include "meguminlink/common.hpp" // IWYU pragma: keep

namespace mlink
{
esp_err_t wireless_link_init();

/** ESP-NOW 广播地址，用于发现阶段的 Probe 广播 */
inline constexpr u8 ESP_NOW_BROADCAST[ESP_NOW_ETH_ALEN] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

namespace payload
{
struct Probe
{
};

struct PairingRequest
{
    u8 target_mac[ESP_NOW_ETH_ALEN];
};

struct PairingAccepted
{
    u8 target_mac[ESP_NOW_ETH_ALEN];
};
} // namespace payload

enum class PacketType
{
    Probe,
    PairingRequest,  /** 配对请求，对方必须回应一个`PairingAccepted`才能完成配对 */
    PairingAccepted, /** 接受配对请求，作为对`PairingRequest`的回应 */
};

union PacketPayload {
    payload::Probe probe;
    payload::PairingRequest pairing_request;
    payload::PairingAccepted pairing_accepted;
};

class Packet
{
  public:
    Packet();

    esp_err_t deserialize(const u8 *data, size_t len);
    esp_err_t serialize(u8 *buffer, size_t len) const;

    /** 序列化后的帧长度，用于发送时告知 ESP-NOW */
    size_t serialized_size() const;

    PacketType packet_type;
    PacketPayload payload;
};

/** 一帧已接收的协议包，附带 ESP-NOW 源 MAC */
struct ReceivedPacket
{
    Packet packet;
    u8 src_mac[ESP_NOW_ETH_ALEN];
};

/** 向指定对端（可为广播地址）发送一帧协议包 */
esp_err_t wireless_link_send(const u8 *dest_mac, const Packet &packet);

/**
 * 登记一个对端 peer（encrypt=false）。
 * 仅登记尚未存在于 peer list 的地址，重复调用是安全的。
 * 广播地址已由 wireless_link_init() 常驻登记，无需也不应重复登记。
 */
esp_err_t wireless_link_peer_add(const u8 *peer_mac);

/** 从 peer list 删除一个对端 peer（配对放弃/完成时归还槽位） */
esp_err_t wireless_link_peer_remove(const u8 *peer_mac);
} // namespace mlink

#endif
