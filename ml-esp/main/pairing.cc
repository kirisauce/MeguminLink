#include "meguminlink/pairing.hpp"
#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "meguminlink/wireless_link.hpp"

using namespace mlink;

void pairing_process(Application &app)
{
    for (auto &packet : app.received_packets)
    {
        switch (packet.packet_type)
        {
        case PacketType::Probe: {
        }
        }
    }
}