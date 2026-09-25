#pragma once
#ifndef __MEGUMINLINK_USB_CAPABILITY_HPP__
#define __MEGUMINLINK_USB_CAPABILITY_HPP__

#include "meguminlink/common.hpp"
#include "sdkconfig.h"

#include <esp_err.h>

#include <cstring>

namespace mlink
{
/**
 * USB 能力的运行时状态与描述符生成：WebUSB / WinUSB 平台能力，以及 HID 兼容模式。
 *
 * 每个能力各有编译期总开关（见 main/Kconfig.projbuild）：关掉的能力不进 BOS、
 * 不响应其厂商请求、不进 USB 配置描述符，运行时也无法启用。编译期启用时，是否
 * 向主机宣告由 NVS 里的运行时状态决定——配置通道（见 config_channel.cc）0x03 读、
 * 0x16 提交草稿、0x17 应用；应用即重建描述符并重新枚举 USB。
 *
 * HID 兼容模式的总开关是 MEGUMINLINK_USB_HID_DAP_ENABLE（决定 CMSIS-DAP v1 传输
 * 路径是否编入），运行状态默认关闭，开启后配置描述符里才有 HID 接口。
 *
 * landing page URL 出现在两处，内容始终一致：BOS 里 WebUSB 平台能力引用的
 * 字符串描述符（iLandingPage）与 GET_URL 厂商请求的响应描述符。上限受
 * esp_tinyusb 字符串描述符缓冲（128 字节）约束。
 *
 * 设备名（device_name）同样走这条通道，但改的是产品名字符串本身，且一律
 * 附加在 CMSIS-DAP 关键字之外（见 USB_PRODUCT_SUFFIX / DAP_PRODUCT_SUFFIX）：
 * 主机的 CMSIS-DAP 自动识别依赖名字里的这个关键字，因此它永远不被自定义名
 * 替换掉。
 */

/** landing page URL 的字符上限（esp_tinyusb 字符串缓冲 128 字节 - 2 字节头 - 结尾 NUL） */
inline constexpr usize USB_LANDING_PAGE_MAX_LEN = 126;

/**
 * 自定义设备名的字符上限。
 *
 * 由产品名描述符倒推：名字拼上固定后缀后必须落在 esp_tinyusb 的单字符串缓冲内
 * （MAX_DESC_BUF_SIZE = 32 字节，含 2 字节描述符头，即最多 31 字符）。
 */
inline constexpr usize USB_DEVICE_NAME_MAX_LEN = 20;

/** 产品名里的固定后缀：CMSIS-DAP 关键字保留在括号内，是主机自动识别的依据 */
inline constexpr char USB_PRODUCT_SUFFIX[] = "(CMSIS-DAP)";

/** DAP_Info 产品名的固定后缀（DAP 规范给的产品名上限是 60 字符，远未触及） */
inline constexpr char DAP_PRODUCT_SUFFIX[] = "(MeguminLink CMSIS-DAP)";

/** esp_tinyusb 单字符串描述符的字符上限（缓冲 32 字节 - 2 字节描述符头） */
inline constexpr usize USB_STRING_DESC_MAX_CHARS = 31;

static_assert(USB_DEVICE_NAME_MAX_LEN + sizeof(USB_PRODUCT_SUFFIX) - 1 <= USB_STRING_DESC_MAX_CHARS,
              "device name plus product suffix exceeds the esp_tinyusb string descriptor limit");

/** BOS 平台能力对应的厂商请求码 */
enum : u8
{
    VENDOR_CODE_MS_OS_20 = 0x20,
    VENDOR_CODE_WEBUSB = 0x22,
};

/** 平台能力的厂商请求操作码（wIndex） */
enum : u16
{
    MS_OS_20_OP_GET_DESCRIPTOR_SET = 0x0007,
    WEBUSB_OP_GET_URL = 0x0002,
};

/** 编译期支持的能力位（GetUsbConfig 的 USB_SUPPORTED） */
enum : u8
{
    USB_CAPABILITY_WEBUSB = 0x01,
    USB_CAPABILITY_WINUSB = 0x02,
    USB_CAPABILITY_HID_COMPAT = 0x04, // HID 兼容模式：配置描述符里含 CMSIS-DAP v1 的 HID 接口
};

/** 草稿被拒的原因（GetUsbConfig 的 USB_DRAFT_REASON） */
enum class UsbConfigReject : u8
{
    None = 0,
    TooLong = 1,
    InvalidChar = 2,
    Unsupported = 3,
};

/** 生效的 USB 能力配置 */
struct UsbCapabilitySettings
{
    u8 webusb = 0;
    u8 winusb = 0;
    u8 hid_compat = 0;
    char landing_page[USB_LANDING_PAGE_MAX_LEN + 1] = {};
    /** 自定义设备名（可打印 ASCII）；空串 = 不用自定义名，产品名回到编译期默认值 */
    char device_name[USB_DEVICE_NAME_MAX_LEN + 1] = {};

    bool operator==(const UsbCapabilitySettings &other) const
    {
        return webusb == other.webusb && winusb == other.winusb && hid_compat == other.hid_compat &&
               strcmp(landing_page, other.landing_page) == 0 && strcmp(device_name, other.device_name) == 0;
    }
};

/**
 * MS OS 2.0 描述符集长度：集合头 10 + 配置子集头 8 + 功能子集头 8 + Compatible ID 20
 * + 注册表属性 128（DeviceInterfaceGUID，见 usb_capability.cc）+ vendor revision 6
 */
inline constexpr u16 USB_MS_OS_DESCRIPTOR_SET_LEN = 180;

/** MS OS 2.0 平台能力圈定的接口号：DAP vendor 接口（UsbDescriptors::DAP_INTERFACE，见 usb_descriptors.hpp） */
inline constexpr u8 USB_CAPABILITY_INTERFACE = 0;

/** 编译期支持的能力位掩码 */
u8 usb_capability_supported_mask();

/** landing page 的编译期默认值（NVS 无记录时的取值） */
const char *usb_landing_page_default();

/** URL 校验：非空、不超上限、全部为可打印 ASCII；返回拒绝原因，None 表示合法 */
UsbConfigReject usb_landing_page_check(const char *url);

/**
 * 设备名校验：不超 USB_DEVICE_NAME_MAX_LEN 字符、全部为可打印 ASCII。
 *
 * 空串合法（表示不用自定义名），nullptr 按空串处理。返回拒绝原因。
 */
UsbConfigReject usb_device_name_check(const char *name);

/**
 * 生成 USB 产品名字符串：有名字时 "<名字>(CMSIS-DAP)"，无名字时回退
 * "CMSIS-DAP on <目标芯片>"。
 *
 * 写入 out（容量 cap，含结尾 NUL），返回写入的字符数（不含 NUL）。CMSIS-DAP
 * 关键字在任何情况下都在结果里。
 */
usize usb_product_string(const char *name, char *out, usize cap);

/** 生成 DAP_Info 的产品名字符串：有名字时 "<名字>(MeguminLink CMSIS-DAP)"，无名字时回退常量 */
usize dap_product_string(const char *name, char *out, usize cap);

/** 整份配置校验（含"该能力编译期是否启用"）：返回拒绝原因 */
UsbConfigReject usb_capability_check(const UsbCapabilitySettings &settings);

/** 从 NVS 载入生效配置（NVS 无记录用编译期默认值；未编译的能力恒为 0） */
UsbCapabilitySettings usb_capability_load();

/** 落盘生效配置 */
esp_err_t usb_capability_store(const UsbCapabilitySettings &settings);

/** 删除全部 USB 能力记录（恢复出厂设置；下次载入即编译期默认值） */
esp_err_t usb_capability_erase();

/**
 * 描述符生成器：按生效状态构建 BOS、GET_URL 响应与 MS OS 2.0 描述符集。
 *
 * 缓冲常驻：TinyUSB 的 tud_control_xfer 只保存指针，DATA 阶段（含多包续传）
 * 仍会回读，不能放在回调栈上。重建只在 USB 断开窗口内进行（见
 * CmsisDapUsb::apply_usb_capability），因此无需额外同步。
 */
class UsbCapabilityDescriptors
{
  public:
    /** 按状态重建全部描述符；landing_page_string_index 为 BOS 里 iLandingPage 的字符串索引 */
    void rebuild(const UsbCapabilitySettings &settings, u8 landing_page_string_index);

    const u8 *bos() const
    {
        return m_bos;
    }

    u16 bos_len() const
    {
        return m_bos_len;
    }

    const u8 *url_descriptor() const
    {
        return m_url;
    }

    u16 url_descriptor_len() const
    {
        return m_url_len;
    }

    const u8 *ms_os_descriptor_set() const;

    u16 ms_os_descriptor_set_len() const
    {
        return USB_MS_OS_DESCRIPTOR_SET_LEN;
    }

  private:
    /** 生成 GET_URL 响应描述符（3 字节头 + URL 正文，scheme 编码进 bScheme） */
    void rebuild_url_descriptor(const char *url);

    /** BOS 缓冲：头 5 + WebUSB 能力 24 + MS OS 2.0 能力 28（最多 57 字节） */
    u8 m_bos[64] = {};
    u16 m_bos_len = 0;

    u8 m_url[3 + USB_LANDING_PAGE_MAX_LEN] = {};
    u16 m_url_len = 0;
};
} // namespace mlink

#endif // __MEGUMINLINK_USB_CAPABILITY_HPP__
