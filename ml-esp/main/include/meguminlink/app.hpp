#ifndef __MEGUMINLINK_APP_HPP__
#define __MEGUMINLINK_APP_HPP__

#include "meguminlink/wireless_link.hpp"
#include <vector>
#include <freertos/FreeRTOS.h>

namespace mlink
{
enum class ApplicationState
{
    Idle,
    Pairing,
};

class Application
{
  public:
    Application();
    Application(const Application &) = delete;

    ApplicationState state() const;
    void set_state(ApplicationState new_state);
    void set_state_from_isr(ApplicationState new_state, BaseType_t *const pxHigherPriorityTaskWoken);

    SemaphoreHandle_t main_task_wake();

    void run();

    static Application &get_instance();

    std::vector<Packet> received_packets;

  private:
    ApplicationState m_state;
    SemaphoreHandle_t m_main_task_wake;

    SemaphoreHandle_t m_packet_received;
};
} // namespace mlink

#endif