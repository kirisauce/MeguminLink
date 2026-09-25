#pragma once
#ifndef __MEGUMINLINK_USB_HPP__
#define __MEGUMINLINK_USB_HPP__

#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/ringbuf.h>
#include <tusb.h>

#include <array>

#include "meguminlink/common.hpp"
#include "meguminlink/dap_executor.hpp"
#include "meguminlink/usb/cdc_acm.hpp"
#include "meguminlink/usb/usb_capability.hpp"
#include "sdkconfig.h"

namespace mlink
{
enum class UsbStringIndex
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
 * USB 配置描述符构建器：接口段按插入顺序拼装、接口号顺延，配置头在收尾时回填。
 *
 * 缓冲常驻：esp_tinyusb 安装时取走它的地址，主机按描述符自身的 wTotalLength
 * 读取，因此重建只改内容、不改地址。重建在 USB 断开窗口内进行（见
 * CmsisDapUsb::apply_usb_capability）。
 */
class UsbConfigurationDescriptor
{
  public:
    /** 描述符首地址 */
    const u8 *data() const
    {
        return m_buffer;
    }

    /** 按生效的 USB 能力重建：HID 兼容模式决定是否插入 HID 接口 */
    void rebuild(const UsbCapabilitySettings &settings);

  private:
    // 端点地址（方向位在最高位）：各接口段的端点不得与其它接口冲突
    static constexpr u8 EP_DAP_OUT = 0x02; // Bulk OUT：命令
    static constexpr u8 EP_DAP_IN = 0x83;  // Bulk IN ：响应

#if CONFIG_MEGUMINLINK_USB_UART_BRIDGE_ENABLE
    static constexpr u8 EP_CDC_NOTIF = 0x84; // Interrupt IN：CDC 通知
    static constexpr u8 EP_CDC_OUT = 0x03;   // Bulk OUT：CDC 数据
    static constexpr u8 EP_CDC_IN = 0x81;    // Bulk IN ：CDC 数据
#endif

#if CONFIG_MEGUMINLINK_USB_HID_DAP_ENABLE
    static constexpr u8 EP_HID_OUT = 0x01; // Interrupt OUT：命令
    static constexpr u8 EP_HID_IN = 0x82;  // Interrupt IN ：响应
#endif

    /** 缓冲容量：编译期最长布局（HID 接口段计入上界，按需插入的接口不会更长） */
    static constexpr usize CAPACITY = TUD_CONFIG_DESC_LEN + TUD_VENDOR_DESC_LEN
#if CONFIG_MEGUMINLINK_USB_UART_BRIDGE_ENABLE
                                      + TUD_CDC_DESC_LEN
#endif
#if CONFIG_MEGUMINLINK_USB_HID_DAP_ENABLE
                                      + TUD_HID_INOUT_DESC_LEN
#endif
        ;

    /** 写入配置头并复位接口号 */
    void begin();

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
    void finish();

    /** 追加一段描述符字节 */
    void append(const u8 *bytes, usize len);

    u8 m_buffer[CAPACITY] = {};
    usize m_len = 0;
    /** 下一个待分配的接口号（连续分配，收尾时即接口数） */
    u8 m_next_interface = 0;
};

/**
 * CMSIS-DAP USB 设备（复合设备）。
 *
 * 仅负责描述符与 TinyUSB 驱动初始化；DAP 命令处理在 usb.cc 的 dap_task 中。
 */
class CmsisDapUsb
{
  public:
    CmsisDapUsb();
    ~CmsisDapUsb();

    /**
     * 安装 TinyUSB 驱动、注册 CMSIS-DAP v2 描述符，并在 UART 透传接口开启时安装
     * CDC ACM 接口（见 CdcAcm）（幂等）。
     *
     * 本机串口的驱动安装与桥任务不在这里：那是 UartBridge 的职责，两端共用
     * 同一实现（见 meguminlink/uart/uart_bridge.hpp），USB 侧只提供 CDC 这一端。
     */
    esp_err_t init();

    /**
     * 启用/禁用整个 USB 设备（任务上下文调用）。
     *
     * disabled：tud_disconnect() 断开 USB 总线（主机端表现为拔线），
     * 并挂起 DAP 任务；enabled：tud_connect() 重新枚举并恢复任务。
     *
     * 注意：当前无调用方——USB 在 Local 与 Wireless 模式下都常驻启用
     * （Wireless 下 DAP 命令由 dap_task 无线转发，见 usb.cc），此接口保留
     * 以备将来按需启停（如省电、仅断开 CDC 等场景）。
     */
    void set_enabled(bool enabled);

    /** 当前生效的 USB 能力配置（BOS 与配置描述符据此构造） */
    const UsbCapabilitySettings &usb_capability() const
    {
        return m_usb_capability;
    }

    /** HID 接口当前是否对主机暴露（HID 兼容模式开启时才在配置描述符里） */
    bool hid_exposed() const
    {
        return m_usb_capability.hid_compat != 0;
    }

    /** DAP_Info 的产品名（含自定义设备名；无名字时为 "MeguminLink CMSIS-DAP"） */
    const char *dap_product_name() const
    {
        return m_dap_product_string;
    }

    /** 描述符访问器：TinyUSB 的 BOS / GET_URL / GET_DESCRIPTOR_SET 回调用它取数据 */
    const UsbCapabilityDescriptors &usb_descriptors() const
    {
        return m_usb_descriptors;
    }

    /**
     * 应用新的 USB 能力配置：断开总线 → 重建描述符（BOS、GET_URL、iLandingPage
     * 字符串与 USB 配置描述符）→ 重新连接，主机重新读取描述符后新配置才可见。
     *
     * 重建放在断开窗口内：此时主机已停止请求描述符，描述符缓冲与字符串内容
     * 不会被 TinyUSB 任务并发读取。NVS 落盘由调用方负责（见 app.cc）。
     */
    void apply_usb_capability(const UsbCapabilitySettings &settings);

    TaskHandle_t m_dap_task = nullptr;
    /**
     * DAP 命令队列（NOSPLIT ringbuffer，元素为 usb.cc 的 DapCommand）：
     * vendor bulk（CMSIS-DAP v2）与 HID（CMSIS-DAP v1）两条传输通道共用，
     * 由 dap_task 顺序消费——DAP 执行器不可重入，命令必须串行执行。
     */
    RingbufHandle_t m_dap_command_buffer;

    /**
     * 跨 USB 包重组缓冲：一条命令可能大于单个 USB 包，接收回调按命令
     * 自描述长度累积，攒够一条才入队。仅 TinyUSB 任务上下文读写。
     */
    u8 m_rx_accum[DAP_MAX_REQUEST_SIZE];
    size_t m_rx_accum_len = 0;

  public:
#if CONFIG_MEGUMINLINK_USB_UART_BRIDGE_ENABLE
    /** UART 透传的 CDC ACM 接口（主机串口）：接口安装与收发都在 CdcAcm 里 */
    CdcAcm &cdc()
    {
        return m_cdc;
    }
#endif

  private:
    bool m_initialized = false;
    char m_serial[18]; // MACSTR 格式 "aa:bb:cc:dd:ee:ff"（17 字符 + NUL）
    std::array<const char *, (usize)UsbStringIndex::Max> m_string_descriptors;

    /**
     * 产品名字符串的常驻缓冲：iProduct 与 DAP_Info 产品名各一份，内容按生效的
     * 设备名生成（见 usb_product_string / dap_product_string）。设备名变更时在
     * 断开窗口内重写，指针不变、内容变——与 iLandingPage 同一套做法。
     */
    char m_product_string[USB_DEVICE_NAME_MAX_LEN + sizeof(USB_PRODUCT_SUFFIX)] = {};
    char m_dap_product_string[USB_DEVICE_NAME_MAX_LEN + sizeof(DAP_PRODUCT_SUFFIX)] = {};

    /**
     * USB 能力状态与描述符缓冲（构造时从 NVS 载入）。
     * m_usb_capability.landing_page 同时是 iLandingPage 字符串描述符的内容来源，
     * m_usb_descriptors 持有 BOS 与 GET_URL 响应的常驻缓冲。
     */
    UsbCapabilitySettings m_usb_capability;
    UsbCapabilityDescriptors m_usb_descriptors;
    /** 配置描述符（HID 兼容模式决定是否含 HID 接口），按上面的能力状态构建 */
    UsbConfigurationDescriptor m_config_descriptor;

#if CONFIG_MEGUMINLINK_USB_UART_BRIDGE_ENABLE
    /** UART 透传的 CDC ACM 接口（主机串口），见 cdc_acm.hpp */
    CdcAcm m_cdc;
#endif
};
} // namespace mlink

#endif
