#ifndef __MEGUMINLINK_APP_HPP__
#define __MEGUMINLINK_APP_HPP__

#include "meguminlink/pairing.hpp"
#include "meguminlink/wireless_link.hpp"
#include "sdkconfig.h"

#if CONFIG_MEGUMINLINK_USB_SUPPORT
#include "meguminlink/usb.hpp"
#endif

#include <freertos/FreeRTOS.h>
#include <vector>

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

    /** 由无线链路接收回调调用：入队一帧数据并唤醒主任务 */
    void push_received_packet(const ReceivedPacket &pkt);

    /** 由主任务调用：取走全部积压帧（返回后本队列为空） */
    std::vector<ReceivedPacket> take_received_packets();

#if CONFIG_MEGUMINLINK_USB_SUPPORT
    CmsisDapUsb &usb();
#endif

    void run();

    static Application &get_instance();

  private:
    ApplicationState m_state;
    SemaphoreHandle_t m_main_task_wake;

    SemaphoreHandle_t m_rx_mutex;
    std::vector<ReceivedPacket> m_received_packets;
    PairingState m_pairing;

#if CONFIG_MEGUMINLINK_USB_SUPPORT
    CmsisDapUsb m_usb;
#endif
};
} // namespace mlink

#endif
