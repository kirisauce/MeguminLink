#ifndef __MEGUMINLINK_APP_HPP__
#define __MEGUMINLINK_APP_HPP__

#include "meguminlink/nvs_flash.hpp"
#include "meguminlink/pairing.hpp"
#include "meguminlink/wireless_link.hpp"
#include "sdkconfig.h"

#if CONFIG_MEGUMINLINK_MODE_SWITCH_ENABLE
#include "meguminlink/button_state.hpp"
#endif

#if CONFIG_MEGUMINLINK_USB_SUPPORT
#include "meguminlink/usb/usb.hpp"
#endif

#if CONFIG_MEGUMINLINK_LED_DRIVER_GPIO
#include "meguminlink/led/led_controller.hpp"
#endif

#include <freertos/FreeRTOS.h>
#include <vector>

namespace mlink
{
/** Local 模式是否可达：USB 支持未编译时（含非 S3 目标）Local 不可达，强制 Wireless */
#if CONFIG_MEGUMINLINK_USB_SUPPORT
inline constexpr bool kLocalModeSupported = true;
#else
inline constexpr bool kLocalModeSupported = false;
#endif

/**
 * Application 运行模式（唯一状态机，见 Application）。
 *
 * Local：本地调试，USB CMSIS-DAP（+ UART 桥）正常工作；仅
 * kLocalModeSupported（USB 支持启用）时可达，否则强制 Wireless；
 * Wireless：无线调试，USB 设备整体禁用，无线逻辑尚未实现（占位）。
 *
 * 配对（ESP-NOW 建链）是 Wireless 的子流程，统一由配对流程管理
 * （enter_pairing / leave_pairing），两个入口共用一个状态：
 * - 自动：Wireless 模式下 NVS 无 peer 记录 → 自动进入配对；
 * - 手动：Wireless 模式下长按 → 强制重新配对（不清 NVS 记录）。
 * 退出：配对成功落盘（peer_mac 写 NVS）即退出；手动进入的另有超时
 * （MEGUMINLINK_PAIRING_MODE_TIMEOUT_MS，默认 60s）/ 任意按键退出，
 * 用于误按后恢复旧配对。自动进入的配对不因超时/按键退出（退出后无旧
 * 配对可用、立即重新进入，无意义），以配对成功为唯一出口。
 * 配对期间长按识别被抑制：自动配对中的长按不会叠加手动配对。
 * Local 模式不涉及配对（USB 调试即插即用）。
 */
enum class Mode
{
    Local,    // 本地调试（USB）；仅 kLocalModeSupported 时可达，否则不可用
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

    /**
     * 配对成功落盘后由 PairingState 回调：统一退出配对流程。
     */
    /** 是否处于配对流程（供 LedController 自行读取） */
    bool pairing_active() const;

    void pairing_completed();

    static Application &get_instance();

  private:
    NvsKeyU8 m_mode_key; /**< 模式 NVS 键（自动加载带缓存） */
    Mode m_mode;         /**< 当前运行模式 */
    SemaphoreHandle_t m_main_task_wake;

    /**
     * 配对流程统一入口（Wireless 子流程）：
     * - manual=false：自动配对（NVS 无 peer 记录时由 run() 调用）；
     * - manual=true：手动强制重新配对（Wireless 下长按触发，不清 NVS 记录）。
     * 已在配对流程中时幂等忽略（自动配对中的长按不会叠加手动配对）。
     * 退出：配对成功 / 手动超时 / 手动任意按键 / 离开 Wireless 模式。
     */
    void enter_pairing(bool manual);

    /**
     * 配对流程统一退出（reason: completed / timeout / button press / mode switch）。
     * 不删除 NVS 记录：有旧记录则恢复无线调试，无记录则下一轮自动重新进入。
     */
    void leave_pairing(const char *reason);

    /** 按当前模式应用系统行为（USB 启用/禁用）；构造与 set_mode 复用 */
    void apply_mode();

    SemaphoreHandle_t m_rx_mutex;
    std::vector<ReceivedPacket> m_received_packets;
    PairingState m_pairing;

    /** 配对流程活动标志（进入置位，成功/超时/按键/切模式后复位） */
    bool m_pairing_active = false;
    /** 是否手动进入（长按）：决定是否启用超时/任意按键退出 */
    bool m_pairing_manual = false;
    /** 手动进入的时刻（超时判定基准，tick） */
    TickType_t m_pairing_manual_start = 0;

#if CONFIG_MEGUMINLINK_MODE_SWITCH_ENABLE
    ButtonState m_button; /**< 模式切换按键状态机（防抖/双击/长按识别） */
#endif

#if CONFIG_MEGUMINLINK_LED_DRIVER_GPIO
    LedController m_leds; /**< LED 指示灯控制器（主循环周期刷新，内部读本对象状态） */
#endif

#if CONFIG_MEGUMINLINK_USB_SUPPORT
    CmsisDapUsb m_usb;
#endif
};
} // namespace mlink

#endif
