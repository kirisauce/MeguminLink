#include "meguminlink/usb/usb_capability.hpp"

#include "meguminlink/nvs_flash.hpp"

#include <esp_check.h>
#include <esp_log.h>
#include <sdkconfig.h>
#include <tusb.h>

#include <algorithm>
#include <cstring>
#include <string_view>

using namespace mlink;

namespace
{
constexpr char TAG[] = "mlink::usb_capability";

constexpr char NVS_KEY_USB_WEBUSB[] = "usb_webusb";
constexpr char NVS_KEY_USB_WINUSB[] = "usb_winusb";
constexpr char NVS_KEY_USB_LANDING_PAGE[] = "usb_landing";

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

mlink::NvsKeyU8 g_usb_webusb_key(NVS_KEY_USB_WEBUSB, USB_WEBUSB_DEFAULT);
mlink::NvsKeyU8 g_usb_winusb_key(NVS_KEY_USB_WINUSB, USB_WINUSB_DEFAULT);
mlink::NvsKeyString<USB_LANDING_PAGE_MAX_LEN> g_usb_landing_page_key(NVS_KEY_USB_LANDING_PAGE, LANDING_PAGE_DEFAULT);

// ---- MS OS 2.0 描述符集（GET_DESCRIPTOR_SET 的响应体）----
// 布局对齐《Microsoft OS 2.0 Descriptors Specification》：集合头 + 配置/
// 功能子集头 + Compatible ID("WINUSB")。本设备是复合设备（含 CDC），裸
// Compatible ID 无接口范围、Windows 不会套用到任何子功能上，必须用子集
// 圈定到 DAP vendor 接口，才会自动绑定 winusb.sys 且不干扰 CDC。

/** dwWindowsVersion：声明由 Windows 8.1+ 处理本描述符集（与 BOS 平台能力一致） */
constexpr u32 MS_OS_20_WINDOWS_VERSION = 0x06030000;

constexpr size_t MS_OS_20_SET_HEADER_LEN = 10;
constexpr size_t MS_OS_20_CONFIG_SUBSET_HEADER_LEN = 8;
constexpr size_t MS_OS_20_FUNCTION_SUBSET_HEADER_LEN = 8;
constexpr size_t MS_OS_20_COMPATIBLE_ID_LEN = 20;

// 零填充辅助
#define MS_OS_20_PAD_4 0x00, 0x00, 0x00, 0x00
#define MS_OS_20_PAD_8 MS_OS_20_PAD_4, MS_OS_20_PAD_4

// 集合头描述符：wLength + 类型 + dwWindowsVersion + 整个集合的 wTotalLength
#define MS_OS_20_DESC_SET_HEADER(_total_len)                                                                           \
    U16_TO_U8S_LE(MS_OS_20_SET_HEADER_LEN), U16_TO_U8S_LE(MS_OS_20_SET_HEADER_DESCRIPTOR),                             \
        U32_TO_U8S_LE(MS_OS_20_WINDOWS_VERSION), U16_TO_U8S_LE(_total_len)

// 配置子集头：wLength + 类型 + bConfigurationValue + 保留 + 子集 wTotalLength
#define MS_OS_20_DESC_CONFIG_SUBSET(_config_value, _total_len)                                                         \
    U16_TO_U8S_LE(MS_OS_20_CONFIG_SUBSET_HEADER_LEN), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_CONFIGURATION),             \
        (_config_value), 0x00, U16_TO_U8S_LE(_total_len)

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

u8 MS_OS_DESCRIPTOR_SET[] = {
    MS_OS_20_DESC_SET_HEADER(MS_OS_20_SET_HEADER_LEN + MS_OS_20_CONFIG_SUBSET_HEADER_LEN +
                             MS_OS_20_FUNCTION_SUBSET_HEADER_LEN + MS_OS_20_COMPATIBLE_ID_LEN),
    MS_OS_20_DESC_CONFIG_SUBSET(1, MS_OS_20_CONFIG_SUBSET_HEADER_LEN + MS_OS_20_FUNCTION_SUBSET_HEADER_LEN +
                                       MS_OS_20_COMPATIBLE_ID_LEN),
    MS_OS_20_DESC_FUNCTION_SUBSET(USB_CAPABILITY_INTERFACE,
                                  MS_OS_20_FUNCTION_SUBSET_HEADER_LEN + MS_OS_20_COMPATIBLE_ID_LEN),
    MS_OS_20_DESC_COMPATIBLE_ID_WINUSB(),
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

    return usb_landing_page_check(settings.landing_page);
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

    g_usb_landing_page_key.get(settings.landing_page, sizeof(settings.landing_page));
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

    if (ret == ESP_OK)
    {
        ret = g_usb_landing_page_key.set(settings.landing_page);
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
        ret = g_usb_landing_page_key.erase();
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
