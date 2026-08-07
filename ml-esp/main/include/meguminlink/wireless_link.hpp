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
    esp_err_t serialize(u8 *buffer, size_t len);

    PacketType packet_type;
    PacketPayload payload;
};
} // namespace mlink

#endif