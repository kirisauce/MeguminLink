#include "meguminlink/pairing.hpp"
#include "freertos/FreeRTOS.h" // IWYU pragma: keep

using namespace mlink;

void pairing_task() {
    vTaskDelay(pdMS_TO_TICKS(500));
}