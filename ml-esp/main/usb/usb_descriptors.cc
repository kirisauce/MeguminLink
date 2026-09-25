#include "meguminlink/usb/usb_descriptors.hpp"

#include <esp_log.h>
#include <esp_mac.h>

#include <cstddef>
#include <cstdio>
#include <cstring>

namespace
{
using namespace mlink;

constexpr char TAG[] = "mlink::usb_descriptors";

// VID/PID：量产前需注册自有 VID/PID。当前沿用 Espressif TinyUSB 默认值：
// 0x303A 为 Espressif VID，0x4020 为 esp_tinyusb vendor 接口默认 PID (0x4000 | 1<<5)
constexpr u16 USB_VID = 0x303A;
constexpr u16 USB_PID = 0x4020;

// 索引 0 为语言 ID（英文），esp_tinyusb 直接拷贝其原始 2 字节
constexpr char LANGID[] = {0x09, 0x04};

// 端点地址（方向位在最高位）：各接口段的端点不得与其它接口冲突
constexpr u8 EP_DAP_OUT = 0x02; // Bulk OUT：命令
constexpr u8 EP_DAP_IN = 0x83;  // Bulk IN ：响应

#if CONFIG_MEGUMINLINK_USB_UART_BRIDGE_ENABLE
constexpr u8 EP_CDC_NOTIF = 0x84; // Interrupt IN：CDC 通知
constexpr u8 EP_CDC_OUT = 0x03;   // Bulk OUT：CDC 数据
constexpr u8 EP_CDC_IN = 0x81;    // Bulk IN ：CDC 数据
#endif

#if CONFIG_MEGUMINLINK_USB_HID_DAP_ENABLE
constexpr u8 EP_HID_OUT = 0x01; // Interrupt OUT：命令
constexpr u8 EP_HID_IN = 0x82;  // Interrupt IN ：响应
#endif

#if CONFIG_MEGUMINLINK_USB_WEBUSB_CAPABILITY_ENABLE
// BOS 的 WebUSB 平台能力里 iLandingPage 指向本对象的 landing page 字符串
constexpr u8 LANDING_PAGE_STRING_INDEX = static_cast<u8>(UsbStringIndex::WebUsbLandingPage);
#else
/** WebUSB 未编译时 BOS 里没有引用 landing page 的能力，索引取值无意义 */
constexpr u8 LANDING_PAGE_STRING_INDEX = 0;
#endif

// MS OS 2.0 描述符集圈定的接口号（usb_capability.cc）必须就是 DAP vendor 接口
static_assert(USB_DAP_INTERFACE == USB_CAPABILITY_INTERFACE);
} // namespace

namespace mlink
{
void UsbDescriptors::rebuild(const UsbCapabilitySettings &settings)
{
    build_device_descriptor();
    build_string_table(settings);
#if CONFIG_MEGUMINLINK_USB_HID_DAP_ENABLE
    build_hid_report_descriptor();
#endif

    begin_configuration();
    add_dap_interface();
#if CONFIG_MEGUMINLINK_USB_UART_BRIDGE_ENABLE
    add_uart_bridge_interface();
#endif
#if CONFIG_MEGUMINLINK_USB_HID_DAP_ENABLE
    if (settings.hid_compat != 0)
    {
        add_hid_interface();
    }
#endif
    finish_configuration();

    // BOS 按运行时的能力状态构造（见 usb_capability.cc）：主机只在枚举时读取，
    // 运行时开关变化靠 apply_usb_capability 的重新枚举让它重读
    m_capability.rebuild(settings, LANDING_PAGE_STRING_INDEX);
}

esp_err_t UsbDescriptors::load_serial()
{
    // 串号 = 芯片 MAC，保证多个 probe 可同时枚举
    u8 mac[6];
    const esp_err_t ret = esp_read_mac(mac, ESP_MAC_EFUSE_FACTORY);
    if (ret != ESP_OK)
    {
        return ret;
    }

    snprintf(m_serial, sizeof(m_serial), MACSTR, MAC2STR(mac));
    return ESP_OK;
}

const u8 *UsbDescriptors::hid_report() const
{
#if CONFIG_MEGUMINLINK_USB_HID_DAP_ENABLE
    return m_hid_report;
#else
    return nullptr;
#endif
}

void UsbDescriptors::build_device_descriptor()
{
    // bDeviceClass = 0：类在接口级定义（vendor specific）
    m_device.bLength = sizeof(tusb_desc_device_t);
    m_device.bDescriptorType = TUSB_DESC_DEVICE;
    m_device.bcdUSB = 0x0210;
    m_device.bDeviceClass = 0x00;
    m_device.bDeviceSubClass = 0x00;
    m_device.bDeviceProtocol = 0x00;
    m_device.bMaxPacketSize0 = static_cast<u8>(CFG_TUD_ENDPOINT0_SIZE);
    m_device.idVendor = USB_VID;
    m_device.idProduct = USB_PID;
    m_device.bcdDevice = 0x0100;
    m_device.iManufacturer = static_cast<u8>(UsbStringIndex::Manufacturer);
    m_device.iProduct = static_cast<u8>(UsbStringIndex::Product);
    m_device.iSerialNumber = static_cast<u8>(UsbStringIndex::SerialNumber);
    m_device.bNumConfigurations = 0x01;
}

void UsbDescriptors::build_string_table(const UsbCapabilitySettings &settings)
{
    usb_product_string(settings.device_name, m_product_string, sizeof(m_product_string));
    dap_product_string(settings.device_name, m_dap_product_string, sizeof(m_dap_product_string));

    m_string_table[(usize)UsbStringIndex::LangId] = LANGID;
    m_string_table[(usize)UsbStringIndex::Manufacturer] = "MeguminLink";
    m_string_table[(usize)UsbStringIndex::Product] = m_product_string;
    m_string_table[(usize)UsbStringIndex::SerialNumber] = m_serial;
    m_string_table[(usize)UsbStringIndex::DapInterfaceName] = "CMSIS-DAP v2";
#if CONFIG_MEGUMINLINK_USB_UART_BRIDGE_ENABLE
    // CDC 接口的 iInterface 必须给出字符串：空槽会让主机的字符串请求吃到 STALL
    m_string_table[(usize)UsbStringIndex::UartBridgeInterfaceName] = "UART Bridge";
#endif
#if CONFIG_MEGUMINLINK_USB_HID_DAP_ENABLE
    m_string_table[(usize)UsbStringIndex::HidInterfaceName] = "CMSIS-DAP v1";
#endif
#if CONFIG_MEGUMINLINK_USB_WEBUSB_CAPABILITY_ENABLE
    m_string_table[(usize)UsbStringIndex::WebUsbLandingPage] = settings.landing_page;
#endif
}

#if CONFIG_MEGUMINLINK_USB_HID_DAP_ENABLE
void UsbDescriptors::build_hid_report_descriptor()
{
    // 报告描述符：vendor-defined usage page (0xFF00)、64B 定长 Input/Output 报告、
    // 无 Report ID——与 DAPLink 参考实现一致。命令经 Output 报告（中断 OUT）下发、
    // 响应经 Input 报告（中断 IN）回传，pyOCD/OpenOCD 的 HID 后端按定长 64B 收发。
    const u8 report[] = {
        0x06, 0x00, 0xFF, // Usage Page (Vendor Defined 0xFF00)
        0x09, 0x01,       // Usage (0x01)
        0xA1, 0x01,       // Collection (Application)
        0x15, 0x00,       //   Logical Minimum (0)
        0x26, 0xFF, 0x00, //   Logical Maximum (255)
        0x75, 0x08,       //   Report Size (8)
        0x95, 0x40,       //   Report Count (64)
        0x09, 0x01,       //   Usage (0x01)
        0x81, 0x02,       //   Input (Data,Var,Abs)
        0x95, 0x40,       //   Report Count (64)
        0x09, 0x01,       //   Usage (0x01)
        0x91, 0x02,       //   Output (Data,Var,Abs)
        0xC0,             // End Collection
    };
    static_assert(sizeof(report) == sizeof(m_hid_report));

    std::memcpy(m_hid_report, report, sizeof(report));
}
#endif

void UsbDescriptors::begin_configuration()
{
    // bNumInterfaces 与 wTotalLength 收尾回填
    constexpr tusb_desc_configuration_t HEADER = {
        .bLength = TUD_CONFIG_DESC_LEN,
        .bDescriptorType = TUSB_DESC_CONFIGURATION,
        .wTotalLength = 0,
        .bNumInterfaces = 0,
        .bConfigurationValue = 1,
        .iConfiguration = 0,
        .bmAttributes = 0x80, // bus powered
        .bMaxPower = 500 / 2, // 单位 2 mA
    };
    static_assert(sizeof(HEADER) == TUD_CONFIG_DESC_LEN);

    m_config_len = 0;
    m_next_interface = 0;
    append(reinterpret_cast<const u8 *>(&HEADER), sizeof(HEADER));
}

void UsbDescriptors::add_dap_interface()
{
    const u8 itf = USB_DAP_INTERFACE;
    m_next_interface = static_cast<u8>(itf + 1);

    const u8 desc[]{
        TUD_VENDOR_DESCRIPTOR(itf, static_cast<u8>(UsbStringIndex::DapInterfaceName), EP_DAP_OUT, EP_DAP_IN, 64)};
    append(desc, sizeof(desc));
}

#if CONFIG_MEGUMINLINK_USB_UART_BRIDGE_ENABLE
void UsbDescriptors::add_uart_bridge_interface()
{
    // CDC 组占两个接口：通信接口（IAD 的 bFirstInterface）与数据接口
    const u8 itf = m_next_interface;
    m_next_interface = static_cast<u8>(itf + 2);

    const u8 desc[]{TUD_CDC_DESCRIPTOR(itf, static_cast<u8>(UsbStringIndex::UartBridgeInterfaceName), EP_CDC_NOTIF, 64,
                                       EP_CDC_OUT, EP_CDC_IN, 64)};
    append(desc, sizeof(desc));
}
#endif

#if CONFIG_MEGUMINLINK_USB_HID_DAP_ENABLE
void UsbDescriptors::add_hid_interface()
{
    const u8 itf = m_next_interface;
    m_next_interface = static_cast<u8>(itf + 1);

    // 报告描述符长度 + 中断端点对（IN/OUT 各 64B，FS 轮询间隔 1ms）
    const u8 desc[]{TUD_HID_INOUT_DESCRIPTOR(itf, static_cast<u8>(UsbStringIndex::HidInterfaceName),
                                             HID_ITF_PROTOCOL_NONE, sizeof(m_hid_report), EP_HID_OUT, EP_HID_IN,
                                             USB_HID_REPORT_SIZE, 1)};
    append(desc, sizeof(desc));
}
#endif

void UsbDescriptors::finish_configuration()
{
    // ESP 目标为小端，与 USB 描述符一致
    const u16 total_len = static_cast<u16>(m_config_len);
    std::memcpy(m_config + offsetof(tusb_desc_configuration_t, wTotalLength), &total_len, sizeof(total_len));
    m_config[offsetof(tusb_desc_configuration_t, bNumInterfaces)] = m_next_interface;
}

void UsbDescriptors::append(const u8 *bytes, usize len)
{
    if (m_config_len + len > sizeof(m_config))
    {
        // 容量已覆盖全部编译期布局：越界说明新接口段没算进 CAPACITY
        ESP_LOGE(TAG, "Configuration descriptor overflow (%u + %u > %u), interface segment dropped",
                 static_cast<unsigned>(m_config_len), static_cast<unsigned>(len),
                 static_cast<unsigned>(sizeof(m_config)));
        return;
    }

    std::memcpy(m_config + m_config_len, bytes, len);
    m_config_len += len;
}
} // namespace mlink
