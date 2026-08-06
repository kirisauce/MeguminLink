#pragma once
#ifndef __MEGUMINLINK_WIRELESS_LINK_HPP__
#define __MEGUMINLINK_WIRELESS_LINK_HPP__

#include "esp_err.h"
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
} // namespace payload

enum class PacketType
{
    Probe,
};

union PacketPayload {
    payload::Probe probe;
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