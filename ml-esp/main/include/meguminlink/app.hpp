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

#include "meguminlink/uart/uart_bridge.hpp"

#include "meguminlink/led/led_driver.hpp"

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

/** 串口出向可靠性在 NVS 中的 key（出厂默认开） */
inline constexpr char NVS_KEY_UART_RELIABLE[] = "uart_rel";

/** 串口出向可靠性的出厂默认值：下载突发不再被令牌桶整帧丢弃 */
inline constexpr u8 UART_RELIABLE_DEFAULT = 1;

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

    /** 一条无线 DAP 响应：带上它所回应的命令请求号，供发送端精确配回挂起的命令 */
    struct WirelessResponse
    {
        u16 req_id;                    /**< 回应的是哪条命令（主机分配的请求号） */
        u16 len;                       /**< 载荷长度 */
        u8 data[MAX_DAP_PAYLOAD_SIZE]; /**< 载荷 */
    };

    /**
     * 主循环/接收回调调用：把收到的无线 DAP 响应连同它回应的命令请求号入队。
     * 队列而不是单缓冲——窗口大于 1 时一次乱序补齐会连续交付多条响应，单缓冲会把
     * 前一条覆盖掉，主机的响应流随之整体错位。
     */
    void push_wireless_response(u16 req_id, const u8 *data, u16 len);

    /**
     * 由 dap_task 调用：取一条已到达的无线 DAP 响应。
     * @param req_id     输出：响应对应的命令请求号
     * @param buf        响应缓冲区（容量需 ≥ MAX_DAP_PAYLOAD_SIZE）
     * @param len        输出：实际响应字节数
     * @param timeout_ms 等待超时；0 = 只取已到达的，不等待
     * @return true=取到；false=超时或队列为空
     */
    bool wait_wireless_response(u16 *req_id, u8 *buf, u16 *len, u32 timeout_ms);

#if CONFIG_MEGUMINLINK_USB_SUPPORT
    CmsisDapUsb &usb();
#endif

    /** 本机串口桥访问器（写本机 UART / 应用线编码） */
    UartBridge &uart_bridge();

    /** 无线链路访问器（配对/转发/执行接线使用） */
    WirelessLink &wireless_link();

    /** 配置/遥测通道访问器（统计挂点累加、EP0 厂商请求处理） */
    ConfigChannel &config_channel();

    /**
     * 本机某个端口收到数据 → 按当前模式选路。Local：CDC 与本机 UART 直通；
     * Wireless：交给出向缓冲（配对、准入与逐帧发送都在链路侧完成）。
     * 调用方：S3 侧 CDC RX 回调与 UART 桥任务、C3 侧 UART 桥任务。
     *
     * @param source 数据来自哪个端口，决定帧类型：UartSource::Host 来自本机 CDC、
     *               对端据此写它的 UART TX；UartSource::Target 来自本机 UART、
     *               对端据此写它的 CDC
     */
    void forward_uart_data_from_port(const u8 *data, size_t len, UartSource source);

    /**
     * 无线收到对端串口帧 → 写本机端口（recv_cb 直通调用）。
     *
     * 与 forward_uart_data_from_port 方向相反，参数含义也不同：这里的 source 是**对端**把它
     * 当作哪个端口的数据，决定本机写 UART TX 还是写 CDC。
     */
    void forward_uart_data_from_wireless(const u8 *data, size_t len, UartSource source);

    /**
     * 本机串口线编码变化 → 按当前模式选路。Local：直接设本机 UART；
     * Wireless：同步给配对 peer，由对端翻译后应用。
     */
    void route_line_coding(u32 baud, u8 data_bits, u8 stop_bits, u8 parity);

    void run();

    /** 是否处于配对流程（LED 场景映射读取） */
    bool pairing_active() const;

    /** 配对成功落盘后由 PairingState 回调：统一退出配对流程 */
    void pairing_completed();

    static Application &get_instance();

  private:
    NvsKeyU8 m_mode_key;         /**< 模式 NVS 键（自动加载带缓存） */
    NvsKeyU8 m_uart_reliable_key; /**< 串口出向可靠性 NVS 键（0=尽力而为 1=可靠） */
    Mode m_mode;                 /**< 当前运行模式 */
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

#if CONFIG_MEGUMINLINK_USB_SUPPORT && CONFIG_MEGUMINLINK_USB_UART_BRIDGE_ENABLE
    /**
     * 无线出向的数据来处（注册给 WirelessLink 的空闲钩子）：把 CDC 收到的字节
     * 搬进出向缓冲。缓冲放不下一整块就停手，端点 NAK 反压主机。
     */
    static void fill_host_uart(void *ctx);
#endif

    /** 当前模式与配对状态对应的设备场景（主循环交给 LED 门面下发） */
    LedRequest led_request() const;

    /** 消费配置通道投递的动作（主循环调用；NVS/wifi/重启都只在这里做） */
    void consume_pending_action();

    /** 清除配对记录并重新开始配对流程（ClearPair 动作） */
    void clear_pairing();

    SemaphoreHandle_t m_rx_mutex;
    std::vector<ReceivedPacket> m_received_packets;
    PairingState m_pairing;

    /**
     * 无线 DAP 响应队列（发送端用）：接收回调把响应连同可靠传输序号入队，
     * dap_task 出队后按序号配回挂起的命令。深度对齐 USB 命令队列（两者都受
     * 在途窗口约束），存不下时丢弃并告警——等待中的命令最终由超时销账。
     */
    QueueHandle_t m_wireless_response_queue = nullptr;

    /** 配对流程活动标志（进入置位，成功/超时/按键/切模式后复位） */
    bool m_pairing_active = false;
    /** 是否手动进入（长按）：决定是否启用超时/任意按键退出 */
    bool m_pairing_manual = false;
    /** 手动进入的时刻（超时判定基准，tick） */
    TickType_t m_pairing_manual_start = 0;

#if CONFIG_MEGUMINLINK_MODE_SWITCH_ENABLE
    ButtonState m_button; /**< 模式切换按键状态机（防抖/双击/长按识别） */
#endif

    LedDriver m_leds; /**< 板载指示灯驱动（主循环每拍上报场景，下发去重由驱动入口负责） */

#if CONFIG_MEGUMINLINK_USB_SUPPORT
    CmsisDapUsb m_usb;
#endif

    /** 本机串口桥（UART ↔ 对侧：无线，或 Local 模式下的主机 CDC）；两端同一实现 */
    UartBridge m_uart_bridge;

    /** 串口出向缓冲满的日志限速时刻（下载突发会连续满，逐块报会把日志刷满） */
    u32 m_uart_drop_log_ms = 0;

    /** 无线链路（ESP-NOW 传输 + peer 管理 + 无线侧 DAP 执行接线，见 wireless/link.hpp） */
    WirelessLink m_wireless_link;

    /** 配置/遥测通道（统计计数器 + EP0 厂商请求处理，见 config_channel.hpp） */
    ConfigChannel m_config_channel;
};

/**
 * 确认当前运行的固件可用：处于待校验（ESP_OTA_IMG_PENDING_VERIFY）状态时把它标记
 * 为有效，取消引导程序的下一次回滚。
 *
 * 启用回滚（CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE，见分区表与 sdkconfig.defaults）
 * 后，新固件第一次启动会被引导程序置为待校验，在确认之前再复位就退回旧槽。因此调用
 * 点必须落在"已经能判定这一版固件可用"的时刻：正常模式在 Application 构造完成之后
 * （NVS、无线、USB、按键与 LED 全部就绪），更新模式在进入时（跑得起更新模式的 USB
 * 就说明镜像至少是活的；且 esp_ota_begin 在待校验状态下会直接拒绝，必须先确认）。
 *
 * 幂等：已经确认过、或引导程序直接启动（状态为 VALID/UNDEFINED）时不做任何事；
 * 未启用回滚的目标（如无 USB 的 ESP32-C3）上为空实现。
 */
void ota_confirm_running_app();
} // namespace mlink

#endif
