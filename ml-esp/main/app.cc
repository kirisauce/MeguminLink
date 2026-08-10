#include "meguminlink/app.hpp"

using namespace mlink;

Application::Application()
    : m_state(ApplicationState::Idle), m_main_task_wake(xSemaphoreCreateBinary()), m_rx_mutex(xSemaphoreCreateMutex())
{
}

ApplicationState Application::state() const
{
    return m_state;
}

void Application::set_state(ApplicationState new_state)
{
    if (new_state == ApplicationState::Pairing)
    {
        m_pairing.reset();
    }
    m_state = new_state;
    xSemaphoreGive(main_task_wake());
}

void Application::set_state_from_isr(ApplicationState new_state, BaseType_t *const pxHigherPriorityTaskWoken)
{
    if (new_state == ApplicationState::Pairing)
    {
        m_pairing.reset();
    }
    m_state = new_state;
    xSemaphoreGiveFromISR(m_main_task_wake, pxHigherPriorityTaskWoken);
}

SemaphoreHandle_t Application::main_task_wake()
{
    return m_main_task_wake;
}

void Application::push_received_packet(const ReceivedPacket &pkt)
{
    xSemaphoreTake(m_rx_mutex, portMAX_DELAY);
    m_received_packets.push_back(pkt);
    xSemaphoreGive(m_rx_mutex);

    // ESP-NOW 接收回调运行在 esp_wifi 任务上下文（非 ISR），用普通 Give
    xSemaphoreGive(m_main_task_wake);
}

std::vector<ReceivedPacket> Application::take_received_packets()
{
    std::vector<ReceivedPacket> packets;
    xSemaphoreTake(m_rx_mutex, portMAX_DELAY);
    packets.swap(m_received_packets);
    xSemaphoreGive(m_rx_mutex);
    return packets;
}

#if CONFIG_MEGUMINLINK_USB_SUPPORT
CmsisDapUsb &Application::usb()
{
    return m_usb;
}
#endif

void Application::run()
{
    while (true)
    {
        xSemaphoreTake(m_main_task_wake, pdMS_TO_TICKS(200));

        switch (state())
        {
        case ApplicationState::Idle: {
            // 丢弃积压的配对帧（已配对状态下不再处理配对协议）
            take_received_packets();
            break;
        }
        case ApplicationState::Pairing: {
            m_pairing.process(*this);
            break;
        }
        }
    }
}

static Application global_app;

Application &Application::get_instance()
{
    return global_app;
}
