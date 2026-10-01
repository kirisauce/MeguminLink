#pragma once
#ifndef __MEGUMINLINK_UPDATE_MODE_HPP__
#define __MEGUMINLINK_UPDATE_MODE_HPP__

#include "meguminlink/common.hpp"
#include "sdkconfig.h"

#include <esp_err.h>
#include <tusb.h>

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
 * 无线、串口桥与 Application，只安装一套最小 USB（一个 vendor specific 接口，无
 * CDC、无 HID）并驱动板载 WS2812 显示红→橙→粉渐变。因此任何可能触及 Application
 * 的 TinyUSB 回调都必须先用 UpdateMode::try_instance() 路由到本对象（见 usb.cc）。
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

/** 更新模式配置描述符容量：配置头 + 一个 vendor 接口段（一对 bulk 端点） */
inline constexpr usize UPDATE_CONFIG_DESCRIPTOR_LEN = TUD_CONFIG_DESC_LEN + TUD_VENDOR_DESC_LEN;

/** 更新模式 vendor 接口的端点地址：与正常模式 DAP 接口同一组（两种模式不会同时枚举） */
inline constexpr u8 UPDATE_EP_OUT = 0x02;
inline constexpr u8 UPDATE_EP_IN = 0x83;

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

    /** 安装最小 USB 与灯效，随后进入渐变循环（不返回） */
    void run();

    // ---- TinyUSB 回调入口（usb.cc 路由）----

    /**
     * EP0 厂商控制请求。维护通道本轮没有厂商协议：不接管即由协议栈对 EP0 发
     * STALL。此路径绝不落到 Application::get_instance()。
     */
    bool usb_control(u8 rhport, u8 stage, const tusb_control_request_t *request);

    /** BOS 描述符：更新模式不宣告平台能力（返回 nullptr，主机收到 STALL） */
    const u8 *usb_bos();

    /** vendor bulk 收到的数据：本轮不解析，读空 FIFO 防主机写阻塞，仅记 DEBUG 日志 */
    void usb_rx(u8 idx);

  private:
    /** 填最小描述符集（device / 字符串 / 配置）并安装 TinyUSB：不建 DAP 任务、不装 CDC */
    esp_err_t init_usb();

    /** 按帧序号下发一帧渐变颜色（WS2812 驱动未选时为空实现） */
    void apply_led_frame(u32 phase);

    /** 常驻描述符缓冲：esp_tinyusb 安装时取走指针，主机枚举按内容读取 */
    tusb_desc_device_t m_device = {};
    std::array<const char *, UPDATE_STRING_COUNT> m_string_table = {};
    /** 串号（MACSTR 格式 "aa:bb:cc:dd:ee:ff"，17 字符 + NUL） */
    char m_serial[18] = {};
    u8 m_config[UPDATE_CONFIG_DESCRIPTOR_LEN] = {};

    bool m_usb_installed = false;

#if CONFIG_MEGUMINLINK_LED_DRIVER_WS2812
    Ws2812LedDriver m_led; /**< 板载 WS2812（渐变节奏由 run 的循环掌握） */
#endif
};
} // namespace mlink

#endif // __MEGUMINLINK_UPDATE_MODE_HPP__
