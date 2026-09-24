#include "meguminlink/usb/usb_capability.hpp"

#include "meguminlink/nvs_flash.hpp"

#include <esp_check.h>
#include <esp_log.h>
#include <sdkconfig.h>
#include <tusb.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string_view>

using namespace mlink;

namespace
{
constexpr char TAG[] = "mlink::usb_capability";

constexpr char NVS_KEY_USB_WEBUSB[] = "usb_webusb";
constexpr char NVS_KEY_USB_WINUSB[] = "usb_winusb";
constexpr char NVS_KEY_USB_HID_COMPAT[] = "usb_hid_compat";
constexpr char NVS_KEY_USB_LANDING_PAGE[] = "usb_landing";
constexpr char NVS_KEY_USB_DEVICE_NAME[] = "usb_name";

#if CONFIG_MEGUMINLINK_USB_WEBUSB_CAPABILITY_ENABLE
constexpr char LANDING_PAGE_DEFAULT[] = CONFIG_MEGUMINLINK_USB_WEBUSB_LANDING_PAGE;
#else
// WebUSB 编译期关闭时 Kconfig 项不存在（depends on 未满足）：landing page 无意义
constexpr char LANDING_PAGE_DEFAULT[] = "";
#endif

static_assert(sizeof(LANDING_PAGE_DEFAULT) - 1 <= USB_LANDING_PAGE_MAX_LEN,
              "Kconfig landing page default exceeds the WebUSB string descriptor limit");

/** WebUSB/WinUSB 默认启用；NVS 无记录时按此初始化，恢复出厂设置后回到这里 */
constexpr u8 USB_WEBUSB_DEFAULT = 1;
constexpr u8 USB_WINUSB_DEFAULT = 1;
/** HID 兼容模式默认关闭：只有需要 CMSIS-DAP v1 的工具才打开 */
constexpr u8 USB_HID_COMPAT_DEFAULT = 0;
/** 设备名默认空：产品名保持编译期的 "CMSIS-DAP on <目标芯片>" */
constexpr char USB_DEVICE_NAME_DEFAULT[] = "";

/** 无自定义名时的 DAP_Info 产品名（与 DAP_config.h 非 USB 分支的常量同一文案） */
constexpr char DAP_PRODUCT_FALLBACK[] = "MeguminLink CMSIS-DAP";

mlink::NvsKeyU8 g_usb_webusb_key(NVS_KEY_USB_WEBUSB, USB_WEBUSB_DEFAULT);
mlink::NvsKeyU8 g_usb_winusb_key(NVS_KEY_USB_WINUSB, USB_WINUSB_DEFAULT);
mlink::NvsKeyU8 g_usb_hid_compat_key(NVS_KEY_USB_HID_COMPAT, USB_HID_COMPAT_DEFAULT);
mlink::NvsKeyString<USB_LANDING_PAGE_MAX_LEN> g_usb_landing_page_key(NVS_KEY_USB_LANDING_PAGE, LANDING_PAGE_DEFAULT);
mlink::NvsKeyString<USB_DEVICE_NAME_MAX_LEN> g_usb_device_name_key(NVS_KEY_USB_DEVICE_NAME, USB_DEVICE_NAME_DEFAULT);

// ---- MS OS 2.0 描述符集（GET_DESCRIPTOR_SET 的响应体）----
// 布局对齐《Microsoft OS 2.0 Descriptors Specification》：集合头 + 配置/
// 功能子集头 + Compatible ID("WINUSB") + 注册表属性(DeviceInterfaceGUID) +
// vendor revision。本设备是复合设备（含 CDC），裸 Compatible ID 无接口范围、
// Windows 不会套用到任何子功能上，必须用子集圈定到 DAP vendor 接口，才会自动
// 绑定 winusb.sys 且不干扰 CDC。
//
// 注册表属性是另一半：Windows 8+ 免驱只要求 Compatible ID，而调试器（Keil /
// OpenOCD / pyOCD）是按 DeviceInterfaceGUID **枚举 CMSIS-DAP v2 设备**的。缺了
// 它，主机侧驱动装上了却没有工具能找到这条通道，主机只能退回 HID——而定长 64B 的
// HID 报告装不下按 bulk 上限算出来的命令。GUID 与配置方式见 ARM 的 CMSIS-DAP
// 文档 dap_firmware.md（"Additional settings for Microsoft Windows"）。

/** dwWindowsVersion：声明由 Windows 8.1+ 处理本描述符集（与 BOS 平台能力一致） */
constexpr u32 MS_OS_20_WINDOWS_VERSION = 0x06030000;

constexpr size_t MS_OS_20_SET_HEADER_LEN = 10;
constexpr size_t MS_OS_20_CONFIG_SUBSET_HEADER_LEN = 8;
constexpr size_t MS_OS_20_FUNCTION_SUBSET_HEADER_LEN = 8;
constexpr size_t MS_OS_20_COMPATIBLE_ID_LEN = 20;
constexpr size_t MS_OS_20_VENDOR_REVISION_LEN = 6;

/** 注册表属性描述符（规范 Table 9）：头 8B + 属性名 + 数据长度 2B + 数据 */
constexpr size_t MS_OS_20_REG_PROPERTY_HEADER_LEN = 8;
/** PropertyName = "DeviceInterfaceGUID\0"：19 字符 + 结尾 NUL，UTF-16LE 共 40B */
constexpr size_t MS_OS_20_REG_PROPERTY_NAME_LEN = 40;
/** PropertyData = GUID 字符串 + 结尾 NUL：38 字符 + 结尾 NUL，UTF-16LE 共 78B */
constexpr size_t MS_OS_20_REG_PROPERTY_DATA_LEN = 78;
constexpr size_t MS_OS_20_REG_PROPERTY_LEN = MS_OS_20_REG_PROPERTY_HEADER_LEN + MS_OS_20_REG_PROPERTY_NAME_LEN + 2 +
                                             MS_OS_20_REG_PROPERTY_DATA_LEN;

/** 注册表属性数据类型：REG_SZ（规范 Table 10） */
constexpr u16 MS_OS_20_REG_SZ = 0x0001;

/** 功能子集长度：子集头 + Compatible ID + 注册表属性 + vendor revision */
constexpr size_t MS_OS_20_FUNCTION_SUBSET_LEN = MS_OS_20_FUNCTION_SUBSET_HEADER_LEN + MS_OS_20_COMPATIBLE_ID_LEN +
                                                MS_OS_20_REG_PROPERTY_LEN + MS_OS_20_VENDOR_REVISION_LEN;

// 零填充辅助
#define MS_OS_20_PAD_4 0x00, 0x00, 0x00, 0x00
#define MS_OS_20_PAD_8 MS_OS_20_PAD_4, MS_OS_20_PAD_4

// 集合头描述符：wLength + 类型 + dwWindowsVersion + 整个集合的 wTotalLength
#define MS_OS_20_DESC_SET_HEADER(_total_len)                                                                           \
    U16_TO_U8S_LE(MS_OS_20_SET_HEADER_LEN), U16_TO_U8S_LE(MS_OS_20_SET_HEADER_DESCRIPTOR),                             \
        U32_TO_U8S_LE(MS_OS_20_WINDOWS_VERSION), U16_TO_U8S_LE(_total_len)

// 配置子集头：wLength + 类型 + 配置索引 + 保留 + 子集 wTotalLength
// Windows 按配置索引匹配子集，设备的默认（唯一）配置即索引 0
#define MS_OS_20_DESC_CONFIG_SUBSET(_config_index, _total_len)                                                         \
    U16_TO_U8S_LE(MS_OS_20_CONFIG_SUBSET_HEADER_LEN), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_CONFIGURATION),             \
        (_config_index), 0x00, U16_TO_U8S_LE(_total_len)

// 功能子集头：wLength + 类型 + bFirstInterface + 保留 + 子集 wTotalLength
#define MS_OS_20_DESC_FUNCTION_SUBSET(_first_interface, _total_len)                                                    \
    U16_TO_U8S_LE(MS_OS_20_FUNCTION_SUBSET_HEADER_LEN), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_FUNCTION),                \
        (_first_interface), 0x00, U16_TO_U8S_LE(_total_len)

// Compatible ID 描述符：CompatibleID="WINUSB"、SubCompatibleID 为空
// （规范 Table 13：总长固定 0x14，无尾部填充）
#define MS_OS_20_DESC_COMPATIBLE_ID_WINUSB()                                                                           \
    U16_TO_U8S_LE(MS_OS_20_COMPATIBLE_ID_LEN), U16_TO_U8S_LE(MS_OS_20_FEATURE_COMPATBLE_ID), 'W', 'I', 'N', 'U', 'S',  \
        'B', 0x00, 0x00, /* CompatibleID */                                                                            \
        MS_OS_20_PAD_8   /* SubCompatibleID */

// 属性名 "DeviceInterfaceGUID\0" 与属性数据（CMSIS-DAP v2 的 WinUSB 接口 GUID）的
// UTF-16LE 字节：ASCII 字符逐字符后跟 0x00，末尾各带一个 UTF-16 的 NUL
#define MS_OS_20_UTF16_PROP_NAME_DEVICE_INTERFACE_GUID                                                                  \
    'D', 0x00, 'e', 0x00, 'v', 0x00, 'i', 0x00, 'c', 0x00, 'e', 0x00, 'I', 0x00, 'n', 0x00, 't', 0x00, 'e', 0x00,         \
        'r', 0x00, 'f', 0x00, 'a', 0x00, 'c', 0x00, 'e', 0x00, 'G', 0x00, 'U', 0x00, 'I', 0x00, 'D', 0x00, 0x00, 0x00

#define MS_OS_20_UTF16_CMSIS_DAP_V2_GUID                                                                                \
    '{', 0x00, 'C', 0x00, 'D', 0x00, 'B', 0x00, '3', 0x00, 'B', 0x00, '5', 0x00, 'A', 0x00, 'D', 0x00, '-', 0x00, '2',   \
        0x00, '9', 0x00, '3', 0x00, 'B', 0x00, '-', 0x00, '4', 0x00, '6', 0x00, '6', 0x00, '3', 0x00, '-', 0x00, 'A',   \
        0x00, 'A', 0x00, '3', 0x00, '6', 0x00, '-', 0x00, '1', 0x00, 'A', 0x00, 'A', 0x00, 'E', 0x00, '4', 0x00, '6',   \
        0x00, '4', 0x00, '6', 0x00, '3', 0x00, '7', 0x00, '7', 0x00, '6', 0x00, '}', 0x00, 0x00, 0x00

// 注册表属性描述符：把 CMSIS-DAP v2 的接口 GUID 注册给主机，调试器据此枚举 v2 设备
#define MS_OS_20_DESC_REG_PROPERTY_DEVICE_INTERFACE_GUID()                                                             \
    U16_TO_U8S_LE(MS_OS_20_REG_PROPERTY_LEN), U16_TO_U8S_LE(MS_OS_20_FEATURE_REG_PROPERTY),                           \
        U16_TO_U8S_LE(MS_OS_20_REG_SZ), U16_TO_U8S_LE(MS_OS_20_REG_PROPERTY_NAME_LEN),                                 \
        MS_OS_20_UTF16_PROP_NAME_DEVICE_INTERFACE_GUID, U16_TO_U8S_LE(MS_OS_20_REG_PROPERTY_DATA_LEN),                 \
        MS_OS_20_UTF16_CMSIS_DAP_V2_GUID

// Vendor revision 描述符：复合设备的每个功能子集都必须携带（规范 Table 19），
// 其数值变化是主机刷新已缓存 MSOS 属性的依据
#define MS_OS_20_DESC_VENDOR_REVISION(_revision)                                                                       \
    U16_TO_U8S_LE(MS_OS_20_VENDOR_REVISION_LEN), U16_TO_U8S_LE(MS_OS_20_FEATURE_VENDOR_REVISION),                     \
        U16_TO_U8S_LE(_revision)

u8 MS_OS_DESCRIPTOR_SET[] = {
    MS_OS_20_DESC_SET_HEADER(MS_OS_20_SET_HEADER_LEN + MS_OS_20_CONFIG_SUBSET_HEADER_LEN + MS_OS_20_FUNCTION_SUBSET_LEN),
    MS_OS_20_DESC_CONFIG_SUBSET(0, MS_OS_20_CONFIG_SUBSET_HEADER_LEN + MS_OS_20_FUNCTION_SUBSET_LEN),
    MS_OS_20_DESC_FUNCTION_SUBSET(USB_CAPABILITY_INTERFACE, MS_OS_20_FUNCTION_SUBSET_LEN),
    MS_OS_20_DESC_COMPATIBLE_ID_WINUSB(),
    MS_OS_20_DESC_REG_PROPERTY_DEVICE_INTERFACE_GUID(),
    MS_OS_20_DESC_VENDOR_REVISION(2),
};

// 长度自洽：BOS 平台能力里的 wDescriptorSetLength 与 USB_MS_OS_DESCRIPTOR_SET_LEN 引用同一长度
static_assert(sizeof(MS_OS_DESCRIPTOR_SET) == USB_MS_OS_DESCRIPTOR_SET_LEN);
} // namespace

u8 mlink::usb_capability_supported_mask()
{
    u8 mask = 0;

#if CONFIG_MEGUMINLINK_USB_WEBUSB_CAPABILITY_ENABLE
    mask |= USB_CAPABILITY_WEBUSB;
#endif

#if CONFIG_MEGUMINLINK_USB_WINUSB_CAPABILITY_ENABLE
    mask |= USB_CAPABILITY_WINUSB;
#endif

#if CONFIG_MEGUMINLINK_USB_HID_DAP_ENABLE
    mask |= USB_CAPABILITY_HID_COMPAT;
#endif

    return mask;
}

const char *mlink::usb_landing_page_default()
{
    return LANDING_PAGE_DEFAULT;
}

UsbConfigReject mlink::usb_landing_page_check(const char *url)
{
    const usize len = strlen(url);
    if (len > USB_LANDING_PAGE_MAX_LEN)
    {
        return UsbConfigReject::TooLong;
    }

    for (usize i = 0; i < len; i++)
    {
        const u8 c = static_cast<u8>(url[i]);
        if (c < 0x20 || c > 0x7E)
        {
            return UsbConfigReject::InvalidChar;
        }
    }

    return UsbConfigReject::None;
}

UsbConfigReject mlink::usb_device_name_check(const char *name)
{
    if (name == nullptr)
    {
        return UsbConfigReject::None; // 没给名字 == 空名字 == 用默认产品名
    }

    const usize len = strlen(name);
    if (len > USB_DEVICE_NAME_MAX_LEN)
    {
        return UsbConfigReject::TooLong;
    }

    for (usize i = 0; i < len; i++)
    {
        const u8 c = static_cast<u8>(name[i]);
        if (c < 0x20 || c > 0x7E)
        {
            // esp_tinyusb 逐字节把字符串塞进 UTF-16 码元：非 ASCII 会显示成乱码，
            // 因此名字只收可打印 ASCII
            return UsbConfigReject::InvalidChar;
        }
    }

    return UsbConfigReject::None;
}

usize mlink::usb_product_string(const char *name, char *out, usize cap)
{
    if (cap == 0)
    {
        return 0;
    }

    int written = 0;
    if (name != nullptr && name[0] != '\0')
    {
        written = snprintf(out, cap, "%s%s", name, USB_PRODUCT_SUFFIX);
    }
    else
    {
        written = snprintf(out, cap, "CMSIS-DAP on %s", CONFIG_IDF_TARGET);
    }

    if (written < 0)
    {
        out[0] = '\0';
        return 0;
    }
    return std::min(static_cast<usize>(written), cap - 1);
}

usize mlink::dap_product_string(const char *name, char *out, usize cap)
{
    if (cap == 0)
    {
        return 0;
    }

    int written = 0;
    if (name != nullptr && name[0] != '\0')
    {
        written = snprintf(out, cap, "%s%s", name, DAP_PRODUCT_SUFFIX);
    }
    else
    {
        written = snprintf(out, cap, "%s", DAP_PRODUCT_FALLBACK);
    }

    if (written < 0)
    {
        out[0] = '\0';
        return 0;
    }
    return std::min(static_cast<usize>(written), cap - 1);
}

UsbConfigReject mlink::usb_capability_check(const UsbCapabilitySettings &settings)
{
    const u8 supported = usb_capability_supported_mask();

    if (settings.webusb != 0 && (supported & USB_CAPABILITY_WEBUSB) == 0)
    {
        return UsbConfigReject::Unsupported;
    }
    if (settings.winusb != 0 && (supported & USB_CAPABILITY_WINUSB) == 0)
    {
        return UsbConfigReject::Unsupported;
    }
    if (settings.hid_compat != 0 && (supported & USB_CAPABILITY_HID_COMPAT) == 0)
    {
        return UsbConfigReject::Unsupported;
    }

    const UsbConfigReject landing_page = usb_landing_page_check(settings.landing_page);
    if (landing_page != UsbConfigReject::None)
    {
        return landing_page;
    }

    return usb_device_name_check(settings.device_name);
}

UsbCapabilitySettings mlink::usb_capability_load()
{
    UsbCapabilitySettings settings;

#if CONFIG_MEGUMINLINK_USB_WEBUSB_CAPABILITY_ENABLE
    settings.webusb = g_usb_webusb_key.get();
#endif

#if CONFIG_MEGUMINLINK_USB_WINUSB_CAPABILITY_ENABLE
    settings.winusb = g_usb_winusb_key.get();
#endif

#if CONFIG_MEGUMINLINK_USB_HID_DAP_ENABLE
    settings.hid_compat = g_usb_hid_compat_key.get();
#endif

    g_usb_landing_page_key.get(settings.landing_page, sizeof(settings.landing_page));
    g_usb_device_name_key.get(settings.device_name, sizeof(settings.device_name));
    return settings;
}

esp_err_t mlink::usb_capability_store(const UsbCapabilitySettings &settings)
{
    esp_err_t ret = ESP_OK;

    // 未编译的能力不落盘：保留旧记录，重新编译启用后仍是用户原先的设置
#if CONFIG_MEGUMINLINK_USB_WEBUSB_CAPABILITY_ENABLE
    ret = g_usb_webusb_key.set(settings.webusb);
#endif

#if CONFIG_MEGUMINLINK_USB_WINUSB_CAPABILITY_ENABLE
    if (ret == ESP_OK)
    {
        ret = g_usb_winusb_key.set(settings.winusb);
    }
#endif

#if CONFIG_MEGUMINLINK_USB_HID_DAP_ENABLE
    if (ret == ESP_OK)
    {
        ret = g_usb_hid_compat_key.set(settings.hid_compat);
    }
#endif

    if (ret == ESP_OK)
    {
        ret = g_usb_landing_page_key.set(settings.landing_page);
    }
    if (ret == ESP_OK)
    {
        ret = g_usb_device_name_key.set(settings.device_name);
    }
    return ret;
}

esp_err_t mlink::usb_capability_erase()
{
    esp_err_t ret = g_usb_webusb_key.erase();
    if (ret == ESP_OK)
    {
        ret = g_usb_winusb_key.erase();
    }
    if (ret == ESP_OK)
    {
        ret = g_usb_hid_compat_key.erase();
    }
    if (ret == ESP_OK)
    {
        ret = g_usb_landing_page_key.erase();
    }
    if (ret == ESP_OK)
    {
        ret = g_usb_device_name_key.erase();
    }

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to erase USB capability records: %s", esp_err_to_name(ret));
    }
    return ret;
}

const u8 *mlink::UsbCapabilityDescriptors::ms_os_descriptor_set() const
{
    return MS_OS_DESCRIPTOR_SET;
}

void mlink::UsbCapabilityDescriptors::rebuild(const UsbCapabilitySettings &settings, u8 landing_page_string_index)
{
    // 运行时状态决定宣告哪些能力；编译期关掉的能力即使 NVS 里是启用也不宣告
#if CONFIG_MEGUMINLINK_USB_WEBUSB_CAPABILITY_ENABLE
    const bool webusb = settings.webusb != 0;
#else
    const bool webusb = false;
#endif

#if CONFIG_MEGUMINLINK_USB_WINUSB_CAPABILITY_ENABLE
    const bool winusb = settings.winusb != 0;
#else
    const bool winusb = false;
#endif

    u16 total_len = TUD_BOS_DESC_LEN;
    u8 caps_num = 0;
    if (webusb)
    {
        total_len += TUD_BOS_WEBUSB_DESC_LEN;
        caps_num++;
    }
    if (winusb)
    {
        total_len += TUD_BOS_MICROSOFT_OS_DESC_LEN;
        caps_num++;
    }

    constexpr u16 BOS_MAX_LEN = TUD_BOS_DESC_LEN
#if CONFIG_MEGUMINLINK_USB_WEBUSB_CAPABILITY_ENABLE
                                + TUD_BOS_WEBUSB_DESC_LEN
#endif
#if CONFIG_MEGUMINLINK_USB_WINUSB_CAPABILITY_ENABLE
                                + TUD_BOS_MICROSOFT_OS_DESC_LEN
#endif
        ;
    static_assert(sizeof(m_bos) >= BOS_MAX_LEN);

    const u8 header[]{TUD_BOS_DESCRIPTOR(total_len, caps_num)};
    std::memcpy(m_bos, header, sizeof(header));
    usize p = sizeof(header);

    if (webusb)
    {
        const u8 capability[]{TUD_BOS_WEBUSB_DESCRIPTOR(VENDOR_CODE_WEBUSB, landing_page_string_index)};
        std::memcpy(m_bos + p, capability, sizeof(capability));
        p += sizeof(capability);
    }

    if (winusb)
    {
        const u8 capability[]{TUD_BOS_MS_OS_20_DESCRIPTOR(USB_MS_OS_DESCRIPTOR_SET_LEN, VENDOR_CODE_MS_OS_20)};
        std::memcpy(m_bos + p, capability, sizeof(capability));
        p += sizeof(capability);
    }

    m_bos_len = static_cast<u16>(p);
    rebuild_url_descriptor(settings.landing_page);
}

void mlink::UsbCapabilityDescriptors::rebuild_url_descriptor(const char *url)
{
    // 编译期默认值由 static_assert 保证不超上限；这里再截断一次兜住 NVS 里的历史值
    const std::string_view page(url);
    const bool https = page.starts_with("https://");
    const bool http = page.starts_with("http://");
    const usize prefix_len = https ? 8 : (http ? 7 : 0);
    const usize body_len = std::min(page.size() - prefix_len, USB_LANDING_PAGE_MAX_LEN);

    m_url[0] = static_cast<u8>(3 + body_len);
    m_url[1] = 0x03; // bDescriptorType: WEBUSB_URL
    m_url[2] = static_cast<u8>(https ? 1 : (http ? 0 : 255));
    std::memcpy(m_url + 3, page.data() + prefix_len, body_len);
    m_url_len = static_cast<u16>(3 + body_len);
}
