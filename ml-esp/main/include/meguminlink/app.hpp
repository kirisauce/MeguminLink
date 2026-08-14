#ifndef __MEGUMINLINK_APP_HPP__
#define __MEGUMINLINK_APP_HPP__

#include "meguminlink/nvs_flash.hpp"
#include "meguminlink/pairing.hpp"
#include "meguminlink/wireless_link.hpp"
#include "sdkconfig.h"

#if CONFIG_MEGUMINLINK_USB_SUPPORT
#include "meguminlink/usb/usb.hpp"
#endif

#include <freertos/FreeRTOS.h>
#include <vector>

namespace mlink
{
/**
 * Application 运行模式（唯一状态机，见 Application）。
 *
 * Local：本地调试，USB CMSIS-DAP（+ UART 桥）正常工作；
 * Wireless：无线调试，USB 设备整体禁用，无线逻辑尚未实现（占位）。
 *
 * 配对（ESP-NOW 建链）是 Wireless 的子流程：Wireless 模式下 NVS 无 peer
 * 记录时自动进入配对，配对完成（peer_mac 落 NVS）后无线调试就绪。
 * Local 模式不涉及配对（USB 调试即插即用）。
 */
enum class Mode
{
    Local,    // 本地调试（USB）
    Wireless, // 无线调试（USB 禁用，未实现；未配对时自动配对）
};

/** 模式在 NVS 中的 key（持久化 Local/Wireless 维度） */
inline constexpr char NVS_KEY_MODE[] = "mode";

class Application
{
  public:
    Application();
    Application(const Application &) = delete;

    /** 当前运行模式（Local/Wireless） */
    Mode mode() const;

    /**
     * 设置运行模式：持久化到 NVS 并应用系统行为（USB 启用/禁用）。
     * 调用前需保证 NVS 与 USB 已初始化（延迟构造保证）。
     */
    void set_mode(Mode new_mode);

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
    NvsKeyU8 m_mode_key; /**< 模式 NVS 键（自动加载带缓存） */
    Mode m_mode;         /**< 当前运行模式 */
    SemaphoreHandle_t m_main_task_wake;

    /** 按当前模式应用系统行为（USB 启用/禁用）；构造与 set_mode 复用 */
    void apply_mode();

    SemaphoreHandle_t m_rx_mutex;
    std::vector<ReceivedPacket> m_received_packets;
    PairingState m_pairing;

#if CONFIG_MEGUMINLINK_USB_SUPPORT
    CmsisDapUsb m_usb;
#endif
};
} // namespace mlink

#endif
