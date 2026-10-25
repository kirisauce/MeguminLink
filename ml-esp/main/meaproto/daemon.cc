#include "meaproto/daemon.hpp"

namespace meaproto
{
u32 random_epoch(void *) noexcept
{
    return esp_random();
}
} // namespace meaproto