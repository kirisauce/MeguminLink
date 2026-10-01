#pragma once
#ifndef __MEGUMINLINK_UPDATE_MODE_HPP__
#define __MEGUMINLINK_UPDATE_MODE_HPP__

#include "meguminlink/common.hpp"
#include "meguminlink/usb/firmware_updater.hpp"
#include "sdkconfig.h"

#include <esp_err.h>
#include <tinyusb_cdc_acm.h>
#include <tusb.h>

#include <freertos/FreeRTOS.h>

#include <array>

#if CONFIG_MEGUMINLINK_LED_DRIVER_WS2812
#include "meguminlink/led/ws2812_led_driver.hpp"
#endif

namespace mlink
{
/**
 * 更新模式（update mode）：正常固件经 Vendor Control 请求软复位后进入的独立启动
 * 路径，供设备自身固件维护与 product 板现场调试使用（省去拆壳按 BOOT）。
 *
 * 它不是 Application 的一个模式，而是 app_main 最前面的分支：进入后不初始化 NVS、
 * 无线、串口桥与 Application，只安装一套最小 USB（一个 CDC ACM 接口，无 vendor、
 * 无 HID、无 CMSIS-DAP）并驱动板载 WS2812。因此任何可能触及 Application 的 TinyUSB
 * 回调都必须先用 UpdateMode::try_instance() 路由到本对象（见 usb.cc）。
 *
 * 那条 CDC 串口跑固件升级协议（FirmwareUpdater，协议见 firmware_updater.cc）：主机
 * 把 app 镜像推进空闲 OTA 槽，两级校验通过后设备自行复位进新固件。灯效随会话阶段
 * 变化：空闲渐变、接收按进度、成功绿、失败红闪。
 *
 * 用 CDC 而不是 vendor 接口是为了主机侧免折腾：CDC ACM 由系统串口驱动接管，主机按
 * 普通串口设备访问（Linux 的 /dev/serial/by-id、Windows 的 COM 口），不需要 udev
 * 规则或 WinUSB 驱动；vendor 接口在没装规则/驱动的机器上根本打不开。
 *
 * 进入请求跨软复位传递（请求标志落在 .noinit 段，见 update_mode.cc），app_main
 * 在最前面消费它。本模式不落盘任何状态：硬复位/断电即回到正常模式。
 */
namespace update_mode
{
/** 请求下次启动进入更新模式（配置通道 EnterUpdate 动作调用） */
void request();

/** 取出进入请求并清除（app_main 最前面调用）；true = 本次启动进入更新模式 */
bool consume_request();
} // namespace update_mode

/** 更新模式字符串描述符的索引：即字符串表下标 */
enum class UpdateStringIndex : u8
{
    LangId = 0,
    Manufacturer,
    Product,
    SerialNumber,
    InterfaceName,
    Max,
};

/** 更新模式字符串描述符表项数 */
inline constexpr usize UPDATE_STRING_COUNT = static_cast<usize>(UpdateStringIndex::Max);

/** 更新模式配置描述符容量：配置头 + 一个 CDC 接口段（通信接口 + 数据接口） */
inline constexpr usize UPDATE_CONFIG_DESCRIPTOR_LEN = TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN;

/** CDC 组占用的接口数：通信接口与数据接口各一个 */
inline constexpr u8 UPDATE_INTERFACE_COUNT = 2;

/** 更新模式 CDC 的端点地址：通知（interrupt IN）与数据（bulk OUT/IN） */
inline constexpr u8 UPDATE_EP_CDC_NOTIF = 0x81;
inline constexpr u8 UPDATE_EP_CDC_OUT = 0x02;
inline constexpr u8 UPDATE_EP_CDC_IN = 0x83;

/** 请求帧队列深度：主机严格停等，留一格冗余吸收主机的重发 */
inline constexpr usize UPDATE_FRAME_QUEUE_DEPTH = 2;

class UpdateMode
{
  public:
    /** 延迟构造单例（app_main 的更新模式分支调用；本对象不依赖 NVS） */
    static UpdateMode &get_instance();

    /**
     * 已进入更新模式时返回本对象，否则返回 nullptr。
     * TinyUSB 回调凭它在 Application 之前分流（见 usb.cc）。
     */
    static UpdateMode *try_instance();

    /** 安装最小 USB 与灯效，随后进入升级协议 + 灯效循环（不返回） */
    void run();

    // ---- TinyUSB 回调入口（usb.cc 路由）----

    /**
     * EP0 厂商控制请求。本模式没有控制请求协议（升级协议走 CDC 数据端点）：
     * 不接管即由协议栈对 EP0 发 STALL。此路径绝不落到 Application::get_instance()。
     */
    bool usb_control(u8 rhport, u8 stage, const tusb_control_request_t *request);

    /** BOS 描述符：更新模式不宣告平台能力（返回 nullptr，主机收到 STALL） */
    const u8 *usb_bos();

  private:
    /**
     * CDC 接收回调（TinyUSB 任务上下文）。esp_tinyusb 只收 C 函数指针，所以是静态
     * 成员，实例从 try_instance() 取——本回调只在本模式下注册，那里必然已构造。
     */
    static void cdc_rx_callback(int itf, cdcacm_event_t *event);

    /** 读空 CDC RX FIFO 并把字节交给 RX 解析 */
    void on_cdc_rx();

    /** 填最小描述符集（device / 字符串 / 配置）并安装 TinyUSB 与 CDC：不建 DAP 任务 */
    esp_err_t init_usb();

    /**
     * RX 解析：把跨 USB 传输到达的字节流拼成整帧，按帧头声明的长度切出去投进队列。
     * 魔数不匹配、或声明长度超过单帧上限时按字节重同步（丢 1 字节再找魔数），
     * 断线重连留下的残包因此能被挤出去。
     */
    void feed_rx(const u8 *data, usize len);

    /** 丢弃 RX 解析缓冲开头的 n 字节（切帧与重同步共用） */
    void drop_rx(usize n);

    /** 处理一条已收齐的请求帧：写 flash、发响应；校验通过则复位进新固件 */
    void handle_frame(const u8 *frame);

    /** 把最近组好的响应帧写进 CDC TX FIFO 并 flush 出去 */
    void send_response();

    /** 按当前会话阶段下发一帧灯效（WS2812 驱动未选时为空实现） */
    void apply_led_frame();

#if CONFIG_MEGUMINLINK_LED_DRIVER_WS2812
    /** 当前会话阶段对应的灯效目标 */
    Ws2812LedTarget led_target();
#endif

    /** 常驻描述符缓冲：esp_tinyusb 安装时取走指针，主机枚举按内容读取 */
    tusb_desc_device_t m_device = {};
    std::array<const char *, UPDATE_STRING_COUNT> m_string_table = {};
    /** 串号（MACSTR 格式 "aa:bb:cc:dd:ee:ff"，17 字符 + NUL） */
    char m_serial[18] = {};
    u8 m_config[UPDATE_CONFIG_DESCRIPTOR_LEN] = {};

    bool m_usb_installed = false;

    /** 升级协议接收端（见 firmware_updater.hpp） */
    FirmwareUpdater m_updater;

    /** 请求帧队列（元素 = 一整帧）：USB 任务只投递，写 flash 在主循环里做 */
    QueueHandle_t m_frame_queue = nullptr;

    /** RX 解析缓冲：跨多次 USB 传输拼一条帧，再按帧头声明的长度切出去 */
    usize m_rx_len = 0;
    u8 m_rx_buf[UPDATE_FRAME_MAX] = {};

    /** 当前处理中的请求帧（从队列取出，不占任务栈） */
    u8 m_frame_buf[UPDATE_FRAME_MAX] = {};

    /** 灯效相位起点：相位按真实时间推进，主任务写 flash 期间节奏也不漂 */
    i64 m_led_epoch_us = 0;

#if CONFIG_MEGUMINLINK_LED_DRIVER_WS2812
    Ws2812LedDriver m_led; /**< 板载 WS2812（渐变节奏由 run 的循环掌握） */
#endif
};
} // namespace mlink

#endif // __MEGUMINLINK_UPDATE_MODE_HPP__
