#pragma once
#ifndef __MEGUMINLINK_APP_HPP__
#define __MEGUMINLINK_APP_HPP__

#include "meguminlink/config_channel.hpp"
#include "meguminlink/nvs_flash.hpp"
#include "meguminlink/wireless/frame.hpp"
#include "meguminlink/wireless/pairing.hpp"
#include "meguminlink/wireless/link.hpp"
#include "sdkconfig.h"

#if CONFIG_MEGUMINLINK_MODE_SWITCH_ENABLE
#include "meguminlink/button_state.hpp"
#endif

#if CONFIG_MEGUMINLINK_USB_SUPPORT
#include "meguminlink/usb/usb.hpp"
#endif

#if !CONFIG_MEGUMINLINK_USB_SUPPORT
#include "meguminlink/wireless/uart.hpp"
#endif

#if !CONFIG_MEGUMINLINK_LED_DRIVER_NONE
#include "meguminlink/led/led_controller.hpp"
#endif

#include <freertos/FreeRTOS.h>
#include <vector>

namespace mlink
{
/** Local 模式是否可达：USB 支持未编译时（含非 S3 目标）Local 不可达，强制 Wireless */
#if CONFIG_MEGUMINLINK_USB_SUPPORT
inline constexpr bool LOCAL_MODE_SUPPORTED = true;
#else
inline constexpr bool LOCAL_MODE_SUPPORTED = false;
#endif

/**
 * Application 运行模式（唯一状态机，见 Application）。
 *
 * Local：本地调试，USB CMSIS-DAP（+ UART 桥）正常工作；仅
 * LOCAL_MODE_SUPPORTED（USB 支持启用）时可达，否则强制 Wireless；
 * Wireless：无线调试，USB 设备保持启用（主机可枚举），USB 收到的
 * DAP 命令由 dap_task 打包为 DapCommand 帧无线转发到配对 peer；
 * 接收端当前仅打日志、不回响应（第一版，无 DAP 执行逻辑）。
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
    Local,    // 本地调试（USB）；仅 LOCAL_MODE_SUPPORTED 时可达，否则不可用
    Wireless, // 无线调试（USB 常驻，DAP 命令无线转发；未配对时自动配对）
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
     * 设置运行模式：持久化到 NVS 并应用系统行为（USB 常驻，模式差异在
     * dap_task 的本地执行/无线转发分流）。调用前需保证 NVS 与 USB 已初始化
     * （延迟构造保证）。
     */
    void set_mode(Mode new_mode);

    SemaphoreHandle_t main_task_wake();

    /** 由无线链路接收回调调用：入队一帧数据并唤醒主任务 */
    void push_received_packet(const ReceivedPacket &pkt);

    /** 由主任务调用：取走全部积压帧（返回后本队列为空） */
    std::vector<ReceivedPacket> take_received_packets();

    /**
     * 主循环调用：存入收到的无线 DAP 响应并唤醒等待者。
     * 发送端 dap_task 转发命令后阻塞等待的正是这个响应（一次一个在途请求）。
     */
    void push_wireless_response(const u8 *data, u16 len);

    /**
     * 由 dap_task 调用：阻塞等待无线 DAP 响应（带超时）。
     * @param buf        响应缓冲区（容量需 ≥ MAX_DAP_PAYLOAD_SIZE）
     * @param len        输出：实际响应字节数
     * @param timeout_ms 超时（MEGUMINLINK_WIRELESS_RESPONSE_TIMEOUT_MS）
     * @return true=成功；false=超时（调用方丢弃，PC 端 debugger 自行重试）
     */
    bool wait_wireless_response(u8 *buf, u16 *len, u32 timeout_ms);

#if CONFIG_MEGUMINLINK_USB_SUPPORT
    CmsisDapUsb &usb();
#endif

    /** 无线链路访问器（配对/转发/执行接线使用） */
    WirelessLink &wireless_link();

    /** 配置/遥测通道访问器（统计挂点累加、EP0 厂商请求处理） */
    ConfigChannel &config_channel();

    /**
     * 本地串口口收到数据 → 打包 UartData 帧发给配对 peer（尽力而为）。
     * 调用方：S3 侧 CDC RX 回调、C3 侧 UART 桥任务。未配对/发送失败时
     * 丢弃（与 DAP 转发一致，PC 端串口无重试语义，丢了就是丢了）。
     */
    void send_uart_data(const u8 *data, size_t len);

    /**
     * 收到无线 UartData 帧 → 写入本地串口（recv_cb 直通调用，esp_wifi
     * 任务上下文）。接收端动作由本机角色决定：USB 侧（S3）写 CDC，
     * 无 USB 侧（C3）写 UART。
     */
    void on_uart_data(const u8 *data, size_t len);

    /**
     * 本地串口线编码变化 → 打包 UartLineCoding 帧发给配对 peer（尽力而为）。
     * 调用方：S3 侧 CDC SET_LINE_CODING 回调（Wireless 模式）。未配对丢弃。
     */
    void send_uart_line_coding(u32 baud, u8 data_bits, u8 stop_bits, u8 parity);

    /**
     * 收到无线 UartLineCoding 帧 → 应用到本地 UART（recv_cb 直通调用）。
     * 仅无 USB 侧（C3）处理；USB 侧（S3）不会收到（目标侧不发），防御忽略。
     */
    void on_uart_line_coding(const payload::UartLineCoding &lc);

    void run();

    /** 是否处于配对流程（供 LedController 自行读取） */
    bool pairing_active() const;

    /** 配对成功落盘后由 PairingState 回调：统一退出配对流程 */
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

    /** 消费配置通道投递的动作（主循环调用；NVS/wifi/重启都只在这里做） */
    void consume_pending_action();

    /** 清除配对记录并重新开始配对流程（ClearPair 动作） */
    void clear_pairing();

    SemaphoreHandle_t m_rx_mutex;
    std::vector<ReceivedPacket> m_received_packets;
    PairingState m_pairing;

    /**
     * 无线 DAP 响应通道（发送端用）：
     * 主循环收到 DapResponse 帧 → push_wireless_response 写入并 give 信号量；
     * dap_task 转发命令后 → wait_wireless_response 等待信号量并读出。
     * 缓冲大小对齐无线帧承载上限（MAX_DAP_PAYLOAD_SIZE）。
     */
    u8 m_wireless_response[MAX_DAP_PAYLOAD_SIZE];
    u16 m_wireless_response_len = 0;
    SemaphoreHandle_t m_wireless_response_sem;   /**< 响应到达信号（二进制） */
    SemaphoreHandle_t m_wireless_response_mutex; /**< 响应缓冲互斥 */

    /** 配对流程活动标志（进入置位，成功/超时/按键/切模式后复位） */
    bool m_pairing_active = false;
    /** 是否手动进入（长按）：决定是否启用超时/任意按键退出 */
    bool m_pairing_manual = false;
    /** 手动进入的时刻（超时判定基准，tick） */
    TickType_t m_pairing_manual_start = 0;

#if CONFIG_MEGUMINLINK_MODE_SWITCH_ENABLE
    ButtonState m_button; /**< 模式切换按键状态机（防抖/双击/长按识别） */
#endif

#if !CONFIG_MEGUMINLINK_LED_DRIVER_NONE
    LedController m_leds; /**< LED 指示灯控制器（主循环周期刷新，内部读本对象状态） */
#endif

#if CONFIG_MEGUMINLINK_USB_SUPPORT
    CmsisDapUsb m_usb;
#endif

#if !CONFIG_MEGUMINLINK_USB_SUPPORT
    /** 目标侧无线 UART 桥（UART <-> ESP-NOW，仅无 USB 端如 C3 编译） */
    WirelessUartBridge m_wireless_uart;
#endif

    /** 无线链路（ESP-NOW 传输 + peer 管理 + 无线侧 DAP 执行接线，见 wireless/link.hpp） */
    WirelessLink m_wireless_link;

    /** 配置/遥测通道（统计计数器 + EP0 厂商请求处理，见 config_channel.hpp） */
    ConfigChannel m_config_channel;
};
} // namespace mlink

#endif
