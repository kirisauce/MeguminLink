#pragma once
#ifndef __MEGUMINLINK_USB_DESCRIPTORS_HPP__
#define __MEGUMINLINK_USB_DESCRIPTORS_HPP__

#include <esp_err.h>
#include <tusb.h>

#include <array>

#include "meguminlink/common.hpp"
#include "meguminlink/dap_frame.hpp"
#include "meguminlink/usb/usb_capability.hpp"
#include "sdkconfig.h"

namespace mlink
{
/** DAP vendor 接口号：接口按插入顺序编号，它永远是第一个（MS OS 2.0 圈定的也是它） */
inline constexpr u8 USB_DAP_INTERFACE = 0;

/**
 * 设备 VID/PID：正常模式的描述符集与维护模式的最小描述符共用同一组值
 * （维护模式靠单接口形态与产品名区分，不给主机侧新增匹配规则）。
 *
 * 量产前需注册自有 VID/PID。当前沿用 Espressif TinyUSB 默认值：
 * 0x303A 为 Espressif VID，0x4020 为 esp_tinyusb vendor 接口默认 PID (0x4000 | 1<<5)。
 */
inline constexpr u16 USB_VID = 0x303A;
inline constexpr u16 USB_PID = 0x4020;

#if CONFIG_MEGUMINLINK_USB_HID_DAP_ENABLE
/**
 * HID 报告长度 = DAP_PACKET_SIZE（64）：CMSIS-DAP v1 的一条命令/响应占满一个
 * 定长报告，短数据补零。数值取自公共常量（无线对端回答 DAP_Info 单包尺寸时也用
 * 它），与 CMSIS-DAP 的 DAP_PACKET_SIZE 的一致性由 usb.cc 的编译期断言保证。
 */
inline constexpr u16 USB_HID_REPORT_SIZE = DAP_HID_PACKET_SIZE;

/** HID 报告描述符的字节数（usage page 0xFF00、64B 定长 Input/Output 报告、无 Report ID） */
inline constexpr usize USB_HID_REPORT_DESCRIPTOR_LEN = 27;
#endif

/** 配置描述符缓冲容量：编译期最长布局（HID 接口段计入上界，按需插入的接口不会更长） */
inline constexpr usize USB_CONFIG_DESCRIPTOR_CAPACITY = TUD_CONFIG_DESC_LEN + TUD_VENDOR_DESC_LEN
#if CONFIG_MEGUMINLINK_USB_UART_BRIDGE_ENABLE
                                                        + TUD_CDC_DESC_LEN
#endif
#if CONFIG_MEGUMINLINK_USB_HID_DAP_ENABLE
                                                        + TUD_HID_INOUT_DESC_LEN
#endif
    ;

/** 字符串描述符索引：即字符串表下标；条件编入的项一律追加在末尾，既有索引保持不变 */
enum class UsbStringIndex : u8
{
    LangId = 0,
    Manufacturer,
    Product,
    SerialNumber,
    DapInterfaceName,
#if CONFIG_MEGUMINLINK_USB_UART_BRIDGE_ENABLE
    UartBridgeInterfaceName,
#endif
#if CONFIG_MEGUMINLINK_USB_HID_DAP_ENABLE
    HidInterfaceName,
#endif
#if CONFIG_MEGUMINLINK_USB_WEBUSB_CAPABILITY_ENABLE
    WebUsbLandingPage,
#endif
    Max,
};

/**
 * USB 描述符集对象：device descriptor、字符串描述符表、configuration descriptor、
 * HID 报告描述符，以及平台能力描述符（BOS / GET_URL / MS OS 2.0，见
 * UsbCapabilityDescriptors）都由本对象持有并构建。
 *
 * 接口结构：vendor specific 接口 (class 0xFF) + bulk 端点承载 CMSIS-DAP v2
 * 命令/响应；CONFIG_MEGUMINLINK_USB_UART_BRIDGE_ENABLE 开启时追加 IAD 组合的
 * CDC ACM 接口作 UART 透传；HID 兼容模式开启（且
 * CONFIG_MEGUMINLINK_USB_HID_DAP_ENABLE=y）时再追加 HID 接口承载 CMSIS-DAP v1。
 * 两条 DAP 传输通道共用一条执行路径（见 usb.cc 的「DAP 命令队列」段）。
 * Product String 必须包含 "CMSIS-DAP"，调试器（pyOCD 等）靠它识别设备。
 *
 * 接口插入顺序即接口号，新接口一律追加在最后，保持既有接口号稳定：
 *   接口 0：vendor specific (0xFF, 0x00, 0x00) —— CMSIS-DAP v2 bulk transport
 *     EP2 Bulk OUT（命令）+ EP3 Bulk IN（响应）
 *   CONFIG_MEGUMINLINK_USB_UART_BRIDGE_ENABLE 开启时追加：
 *   IAD 组合的 CDC ACM 串口（UART 透传）
 *     EP4 Interrupt IN（通知）+ EP3 Bulk OUT / EP1 Bulk IN（数据）
 *   HID 兼容模式开启（要求 CONFIG_MEGUMINLINK_USB_HID_DAP_ENABLE=y）时追加：
 *   HID 接口（vendor-defined 64B 报告）—— CMSIS-DAP v1
 *     EP1 Interrupt OUT（命令）+ EP2 Interrupt IN（响应）
 *
 * 缓冲常驻：esp_tinyusb 安装时取走它们的地址，主机按描述符自身的 wTotalLength
 * 读取，因此重建只改内容、不改地址。重建在 USB 断开窗口内进行（见
 * CmsisDapUsb::apply_usb_capability）。
 */
class UsbDescriptors
{
  public:
    /**
     * 按生效的能力配置重建全部描述符：产品名与 landing page 字符串内容、BOS
     * （含 iLandingPage 索引）与配置描述符的接口组合（HID 兼容模式决定是否含
     * HID 接口）都随 settings 更新。
     */
    void rebuild(const UsbCapabilitySettings &settings);

    /** 读取芯片 MAC 填 iSerialNumber 的内容（缓冲指针不变；esp_tinyusb 安装前调用） */
    esp_err_t load_serial();

    /** Device descriptor（esp_tinyusb 安装时取走指针，须常驻） */
    const tusb_desc_device_t &device() const
    {
        return m_device;
    }

    /** 配置描述符首地址 */
    const u8 *config() const
    {
        return m_config;
    }

    /**
     * 字符串描述符表。
     *
     * tinyusb_desc_config_t::string 是非 const 指针，而表内容只读，故本访问器为
     * 非 const 成员，只在 TinyUSB 安装前的初始化上下文调用。
     */
    const char **string_table()
    {
        return m_string_table.data();
    }

    int string_count() const
    {
        return static_cast<int>(m_string_table.size());
    }

    /**
     * HID 报告描述符：vendor-defined usage page (0xFF00)、64B 定长 Input/Output
     * 报告、无 Report ID——与 DAPLink 参考实现一致。
     *
     * HID 接口未暴露时（MEGUMINLINK_USB_HID_DAP_ENABLE=n）为 nullptr：描述符里没有
     * HID 接口，TinyUSB 不会调用报告描述符回调。
     */
    const u8 *hid_report() const;

    /** 平台能力描述符：BOS、GET_URL 响应与 MS OS 2.0 描述符集 */
    const UsbCapabilityDescriptors &capability() const
    {
        return m_capability;
    }

    /** DAP_Info 的产品名（含自定义设备名；无名字时为 "MeguminLink CMSIS-DAP"） */
    const char *dap_product_name() const
    {
        return m_dap_product_string;
    }

  private:
    /** 填 device descriptor（VID/PID、类与包大小固定，字符串索引取自 UsbStringIndex） */
    void build_device_descriptor();

    /** 按生效的设备名生成产品名与 DAP 产品名，并装配字符串描述符表 */
    void build_string_table(const UsbCapabilitySettings &settings);

    /** 填 HID 报告描述符（内容固定：vendor-defined 64B 定长报告） */
    void build_hid_report_descriptor();

    /** 写入配置头并复位缓冲游标与接口号 */
    void begin_configuration();

    /** 追加 CMSIS-DAP v2 的 vendor specific 接口（bulk 端点对） */
    void add_dap_interface();

#if CONFIG_MEGUMINLINK_USB_UART_BRIDGE_ENABLE
    /** 追加 UART 透传的 CDC ACM 接口组（IAD + 通信接口 + 数据接口） */
    void add_uart_bridge_interface();
#endif

#if CONFIG_MEGUMINLINK_USB_HID_DAP_ENABLE
    /** 追加 CMSIS-DAP v1 的 HID 接口（中断端点对） */
    void add_hid_interface();
#endif

    /** 回填配置头的 bNumInterfaces 与 wTotalLength */
    void finish_configuration();

    /** 追加一段描述符字节 */
    void append(const u8 *bytes, usize len);

    tusb_desc_device_t m_device = {};
    std::array<const char *, (usize)UsbStringIndex::Max> m_string_table = {};

    /** 串号（MACSTR 格式 "aa:bb:cc:dd:ee:ff"，17 字符 + NUL） */
    char m_serial[18] = {};

    /**
     * 产品名字符串的常驻缓冲：iProduct 与 DAP_Info 产品名各一份，内容按生效的
     * 设备名生成（见 usb_product_string / dap_product_string）。设备名变更时在
     * 断开窗口内重写，指针不变、内容变——与 iLandingPage 字符串同一套做法。
     */
    char m_product_string[USB_DEVICE_NAME_MAX_LEN + sizeof(USB_PRODUCT_SUFFIX)] = {};
    char m_dap_product_string[USB_DEVICE_NAME_MAX_LEN + sizeof(DAP_PRODUCT_SUFFIX)] = {};

#if CONFIG_MEGUMINLINK_USB_HID_DAP_ENABLE
    /** HID 报告描述符本体（esp_tinyusb 取走指针，重建时重填、地址不变） */
    u8 m_hid_report[USB_HID_REPORT_DESCRIPTOR_LEN] = {};
#endif

    u8 m_config[USB_CONFIG_DESCRIPTOR_CAPACITY] = {};
    usize m_config_len = 0;
    /** 下一个待分配的接口号（连续分配，收尾时即接口数） */
    u8 m_next_interface = 0;

    /** BOS、GET_URL 响应与 MS OS 2.0 描述符集的常驻缓冲 */
    UsbCapabilityDescriptors m_capability;
};
} // namespace mlink

#endif // __MEGUMINLINK_USB_DESCRIPTORS_HPP__
