#include "meaproto/hash.hpp"

namespace meaproto
{
namespace
{
constexpr u32 FNV_OFFSET_BASIS = 0x811C9DC5u;
constexpr u32 FNV_PRIME = 0x01000193u;
} // namespace

u16 config_hash(std::span<const ChannelSpec> channels) noexcept
{
    u32 hash = FNV_OFFSET_BASIS;
    for (const ChannelSpec &spec : channels)
    {
        const u8 fields[] = {
            spec.id.value,
            static_cast<u8>(spec.reliability),
            static_cast<u8>(spec.max_payload >> 8),
            static_cast<u8>(spec.max_payload & 0xFF),
        };
        for (const u8 field : fields)
        {
            hash ^= field;
            hash *= FNV_PRIME;
        }
    }
    return static_cast<u16>(hash ^ (hash >> 16));
}
} // namespace meaproto
