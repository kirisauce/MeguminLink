#include "meguminlink/usb/usb.hpp"
#include "meguminlink/app.hpp"
#include "meguminlink/dap_executor.hpp"  // dap_executor_run（Local 执行核心）
#include "meguminlink/wireless_link.hpp" // WirelessLink::forward_dap_command（Wireless 转发）

#include <esp_check.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_rom_sys.h> // esp_rom_delay_us
#include <sdkconfig.h>
#include <tinyusb.h>
#include <tinyusb_default_config.h>
#include <tusb.h>

#include <cstdio>
#include <cstring>
#include <string_view>

// 注意 include 顺序：DAP_config.h 必须先于 DAP.h（与 cmsis_dap/Source/*.c 一致）。
// DAP_config.h 提供 DELAY_SLOW_CYCLES/PIN_DELAY_SLOW 等平台实现，
// DAP.h 仅在未定义时提供默认值（#ifndef 保护）；顺序反了会触发宏重定义。
// clang-format 的字典序排序会把它们排反，此处保持顺序：
// clang-format off
#include "DAP_config.h"
#include "DAP.h" // IWYU pragma: keep
// clang-format on

static constexpr char TAG[] = "mlink::usb";

namespace
{
using namespace mlink;

// ---- CMSIS-DAP v2 descriptors ----
// 接口结构：vendor specific 接口 (class 0xFF) + bulk 端点 承载 CMSIS-DAP v2
// 命令/响应；CONFIG_MEGUMINLINK_USB_UART_BRIDGE_ENABLE 开启时追加 IAD 组合的
// CDC ACM 接口作 UART 透传。
// Product String 必须包含 "CMSIS-DAP"，调试器（pyOCD 等）靠它识别设备。

// VID/PID：量产前需注册自有 VID/PID。当前沿用 Espressif TinyUSB 默认值：
// 0x303A 为 Espressif VID，0x4020 为 esp_tinyusb vendor 接口默认 PID (0x4000 | 1<<5)
constexpr u16 USB_VID = 0x303A;
constexpr u16 USB_PID = 0x4020;

// 索引 0 为语言 ID（英文），esp_tinyusb 直接拷贝其原始 2 字节
constexpr char LANGID[] = {0x09, 0x04};

// ---- Device descriptor ----
// bDeviceClass = 0：类在接口级定义（vendor specific）
constexpr tusb_desc_device_t DEVICE_DESCRIPTOR = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0210,
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USB_VID,
    .idProduct = USB_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = (u8)UsbStringIndex::Manufacturer,
    .iProduct = (u8)UsbStringIndex::Product,
    .iSerialNumber = (u8)UsbStringIndex::SerialNumber,
    .bNumConfigurations = 0x01,
};

// ---- Configuration descriptor ----
// 接口结构：
//   接口 0：vendor specific (0xFF, 0x00, 0x00) —— CMSIS-DAP bulk transport
//     EP2 Bulk OUT（命令）+ EP3 Bulk IN（响应）
//   CONFIG_MEGUMINLINK_USB_UART_BRIDGE_ENABLE 开启时追加：
//   接口 1/2：IAD 组合的 CDC ACM 串口（UART 透传）
//     EP4 Interrupt IN（通知）+ EP3 Bulk OUT / EP1 Bulk IN（数据）
enum
{
    ITF_NUM_DAP = 0,
#if CONFIG_MEGUMINLINK_USB_UART_BRIDGE_ENABLE
    ITF_NUM_CDC, // CDC ACM（通知接口）
    ITF_NUM_CDC_DATA,
#endif
    ITF_NUM_TOTAL,

    EP_DAP_OUT = 0x02, // Bulk OUT：命令
    EP_DAP_IN = 0x83,  // Bulk IN ：响应

#if CONFIG_MEGUMINLINK_USB_UART_BRIDGE_ENABLE
    EP_CDC_NOTIF = 0x84, // Interrupt IN：CDC 通知
    EP_CDC_OUT = 0x03,   // Bulk OUT：CDC 数据
    EP_CDC_IN = 0x81,    // Bulk IN ：CDC 数据
#endif
};

// bmAttributes = 0x80：bus powered
constexpr u8 CONFIG_DESCRIPTOR[] = {
#if CONFIG_MEGUMINLINK_USB_UART_BRIDGE_ENABLE
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, TUD_CONFIG_DESC_LEN + TUD_VENDOR_DESC_LEN + TUD_CDC_DESC_LEN, 0x80, 500),
#else
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, TUD_CONFIG_DESC_LEN + TUD_VENDOR_DESC_LEN, 0x80, 500),
#endif

    TUD_VENDOR_DESCRIPTOR(ITF_NUM_DAP, (u8)UsbStringIndex::DapInterfaceName, EP_DAP_OUT, EP_DAP_IN, 64),
#if CONFIG_MEGUMINLINK_USB_UART_BRIDGE_ENABLE
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, (u8)UsbStringIndex::UartBridgeInterfaceName, EP_CDC_NOTIF, 64, EP_CDC_OUT,
                       EP_CDC_IN, 64),
#endif

    // 如需 SWO 流，在此追加 EP3 Bulk In (0x83, 64B) 描述符
};

enum
{
    VENDOR_CODE_MS_OS_20 = 0x20,
    VENDOR_CODE_WEBUSB = 0x22,
};

enum
{
    MS_OS_20_OP_GET_DESCRIPTOR_SET = 0x0007,
    WEBUSB_OP_GET_URL = 0x0002,
};

#if CONFIG_MEGUMINLINK_USB_WINUSB_CAPABILITY_ENABLE

// ---- MS OS 2.0 描述符集（GET_DESCRIPTOR_SET 的响应体）----
// 布局对齐《Microsoft OS 2.0 Descriptors Specification》：集合头 + 配置/
// 功能子集头 + Compatible ID("WINUSB")。本设备是复合设备（含 CDC），裸
// Compatible ID 无接口范围、Windows 不会套用到任何子功能上，必须用子集
// 圈定到 DAP vendor 接口，才会自动绑定 winusb.sys 且不干扰 CDC。

// dwWindowsVersion：声明由 Windows 8.1+ 处理本描述符集（与 BOS 平台能力一致）
constexpr u32 MS_OS_20_WINDOWS_VERSION = 0x06030000;

constexpr size_t MS_OS_20_SET_HEADER_LEN = 10;
constexpr size_t MS_OS_20_CONFIG_SUBSET_HEADER_LEN = 8;
constexpr size_t MS_OS_20_FUNCTION_SUBSET_HEADER_LEN = 8;
constexpr size_t MS_OS_20_COMPATIBLE_ID_LEN = 20;

// 零填充辅助
#define MS_OS_20_PAD_4 0x00, 0x00, 0x00, 0x00
#define MS_OS_20_PAD_8 MS_OS_20_PAD_4, MS_OS_20_PAD_4

// 集合头描述符：wLength + 类型 + dwWindowsVersion + 整个集合的 wTotalLength
#define MS_OS_20_DESC_SET_HEADER(_total_len)                                                          \
    U16_TO_U8S_LE(MS_OS_20_SET_HEADER_LEN), U16_TO_U8S_LE(MS_OS_20_SET_HEADER_DESCRIPTOR),            \
        U32_TO_U8S_LE(MS_OS_20_WINDOWS_VERSION), U16_TO_U8S_LE(_total_len)

// 配置子集头：wLength + 类型 + bConfigurationValue + 保留 + 子集 wTotalLength
#define MS_OS_20_DESC_CONFIG_SUBSET(_config_value, _total_len)                                        \
    U16_TO_U8S_LE(MS_OS_20_CONFIG_SUBSET_HEADER_LEN),                                                 \
        U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_CONFIGURATION), (_config_value), 0x00,                   \
        U16_TO_U8S_LE(_total_len)

// 功能子集头：wLength + 类型 + bFirstInterface + 保留 + 子集 wTotalLength
#define MS_OS_20_DESC_FUNCTION_SUBSET(_first_interface, _total_len)                                   \
    U16_TO_U8S_LE(MS_OS_20_FUNCTION_SUBSET_HEADER_LEN),                                               \
        U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_FUNCTION), (_first_interface), 0x00,                     \
        U16_TO_U8S_LE(_total_len)

// Compatible ID 描述符：CompatibleID="WINUSB"、SubCompatibleID 为空
// （规范 Table 13：总长固定 0x14，无尾部填充）
#define MS_OS_20_DESC_COMPATIBLE_ID_WINUSB()                                                          \
    U16_TO_U8S_LE(MS_OS_20_COMPATIBLE_ID_LEN), U16_TO_U8S_LE(MS_OS_20_FEATURE_COMPATBLE_ID),          \
        'W', 'I', 'N', 'U', 'S', 'B', 0x00, 0x00, /* CompatibleID */                                  \
        MS_OS_20_PAD_8                            /* SubCompatibleID */

u8 MS_OS_20_DESCRIPTOR_SET[] = {
    MS_OS_20_DESC_SET_HEADER(MS_OS_20_SET_HEADER_LEN + MS_OS_20_CONFIG_SUBSET_HEADER_LEN +
                             MS_OS_20_FUNCTION_SUBSET_HEADER_LEN + MS_OS_20_COMPATIBLE_ID_LEN),
    MS_OS_20_DESC_CONFIG_SUBSET(1,
                                MS_OS_20_CONFIG_SUBSET_HEADER_LEN + MS_OS_20_FUNCTION_SUBSET_HEADER_LEN +
                                    MS_OS_20_COMPATIBLE_ID_LEN),
    MS_OS_20_DESC_FUNCTION_SUBSET(ITF_NUM_DAP, MS_OS_20_FUNCTION_SUBSET_HEADER_LEN + MS_OS_20_COMPATIBLE_ID_LEN),
    MS_OS_20_DESC_COMPATIBLE_ID_WINUSB(),
};

// 长度自洽：BOS 平台能力里的 wDescriptorSetLength 引用 sizeof 本数组
static_assert(sizeof(MS_OS_20_DESCRIPTOR_SET) == MS_OS_20_SET_HEADER_LEN + MS_OS_20_CONFIG_SUBSET_HEADER_LEN +
                                                     MS_OS_20_FUNCTION_SUBSET_HEADER_LEN +
                                                     MS_OS_20_COMPATIBLE_ID_LEN);

#endif

consteval u8 bos_descriptor_total_len()
{
    u8 len = TUD_BOS_DESC_LEN;

#if CONFIG_MEGUMINLINK_USB_WEBUSB_CAPABILITY_ENABLE
    len += TUD_BOS_WEBUSB_DESC_LEN;
#endif

#if CONFIG_MEGUMINLINK_USB_WINUSB_CAPABILITY_ENABLE
    len += TUD_BOS_MICROSOFT_OS_DESC_LEN;
#endif

    return len;
}

consteval u8 bos_descriptors_num()
{
    u8 num = 0;

#if CONFIG_MEGUMINLINK_USB_WEBUSB_CAPABILITY_ENABLE
    num += 1;
#endif

#if CONFIG_MEGUMINLINK_USB_WINUSB_CAPABILITY_ENABLE
    num += 1;
#endif

    return num;
}

constexpr u8 BOS_DESCRIPTOR[] = {
    TUD_BOS_DESCRIPTOR(bos_descriptor_total_len(), bos_descriptors_num()),

#if CONFIG_MEGUMINLINK_USB_WEBUSB_CAPABILITY_ENABLE
    TUD_BOS_WEBUSB_DESCRIPTOR(VENDOR_CODE_WEBUSB, static_cast<u8>(UsbStringIndex::WebUsbLandingPage)),
#endif

#if CONFIG_MEGUMINLINK_USB_WINUSB_CAPABILITY_ENABLE
    TUD_BOS_MS_OS_20_DESCRIPTOR(sizeof(MS_OS_20_DESCRIPTOR_SET), VENDOR_CODE_MS_OS_20),
#endif
};

static_assert(sizeof(BOS_DESCRIPTOR) == bos_descriptor_total_len());

#if CONFIG_MEGUMINLINK_USB_WEBUSB_CAPABILITY_ENABLE
// WebUSB URL Descriptor（GET_URL 的响应体）：3 字节头 bLength /
// bDescriptorType(WEBUSB_URL = 0x03) / bScheme + URL 文本。编译期从 Kconfig
// 字符串生成：带 "https://" 或 "http://" 前缀时剥离前缀并把前缀编码进
// bScheme（1/0），其余形式按完整 URL 处理（bScheme = 255）。
constexpr std::string_view WEBUSB_LANDING_PAGE = CONFIG_MEGUMINLINK_USB_WEBUSB_LANDING_PAGE;
constexpr bool WEBUSB_URL_IS_HTTPS = WEBUSB_LANDING_PAGE.starts_with("https://");
constexpr bool WEBUSB_URL_IS_HTTP = WEBUSB_LANDING_PAGE.starts_with("http://");
constexpr size_t WEBUSB_URL_PREFIX_LEN = WEBUSB_URL_IS_HTTPS ? 8 : WEBUSB_URL_IS_HTTP ? 7 : 0;
constexpr u8 WEBUSB_URL_SCHEME = WEBUSB_URL_IS_HTTPS ? 1 : WEBUSB_URL_IS_HTTP ? 0 : 255;
constexpr size_t WEBUSB_URL_BODY_LEN = WEBUSB_LANDING_PAGE.size() - WEBUSB_URL_PREFIX_LEN;

consteval std::array<u8, 3 + WEBUSB_URL_BODY_LEN> make_webusb_url_descriptor()
{
    std::array<u8, 3 + WEBUSB_URL_BODY_LEN> desc{};
    desc[0] = static_cast<u8>(desc.size());
    desc[1] = 0x03; // bDescriptorType: WEBUSB_URL
    desc[2] = WEBUSB_URL_SCHEME;
    for (size_t i = 0; i < WEBUSB_URL_BODY_LEN; i++)
    {
        desc[3 + i] = static_cast<u8>(WEBUSB_LANDING_PAGE[WEBUSB_URL_PREFIX_LEN + i]);
    }
    return desc;
}

// 静态存储：tud_control_xfer 只保存指针，DATA 阶段（含多包续传）仍会读取，
// 不能放在回调栈上。
constexpr auto WEBUSB_URL_DESCRIPTOR = make_webusb_url_descriptor();
#endif

void tinyusb_event_callback([[maybe_unused]] tinyusb_event_t *event, [[maybe_unused]] void *arg)
{
}

// ---- CMSIS-DAP v2 协议处理 ----
// 统一由公共执行器 dap_executor_run（见 dap_executor.cc）驱动：
// 命令源 = USB bulk RX ringbuffer（Wireless 模式在源内完成转发分流），
// 响应宿 = USB vendor TX FIFO。本任务无独立执行循环。

// 请求/响应缓冲为 USB 传输层资源（不占任务栈——ESP-IDF 任务栈以字节计，
// DAP 执行调用链栈需求大，缓冲放栈上会溢出）。
u8 dap_request[DAP_PACKET_SIZE];
// 响应缓冲按 CMSIS-DAP 批量响应上限预留（DAP_MAX_RESPONSE_SIZE，见
// dap_executor.hpp）：Local 模式单命令响应 ≤ DAP_PACKET_SIZE，0x7F 批量
// 响应可到 ~2KB（DAP_ExecuteCommand 批量写入不检查边界）；Wireless 模式
// 等待到的无线响应（≤ MAX_DAP_PAYLOAD_SIZE）同样拷入此缓冲再写回 USB。
u8 dap_response[mlink::DAP_MAX_RESPONSE_SIZE];

// 响应写入 vendor TX FIFO：write 同步拷贝入 FIFO，满时返回 0；
// tinyusb_task 后台排空。拷贝完成即缓冲区可复用，无需等发送完毕。
// FIFO 满的根因：上一次 IN 传输的完成回调（TinyUSB 任务处理）相对本任务
// 滞后，endpoint 仍 busy，数据留在 FIFO。此时 vTaskDelay(1) 的粒度是
// 1 个 tick（此前 100Hz = 10ms！），每次响应都白等一个 tick，读吞吐被压到
// ~5KB/s。改用 us 级忙等重试，只有长时间排不空才兜底让出。
void dap_send_response(const u8 *data, u16 len)
{
    u16 sent = 0;
    while (sent < len)
    {
        if (!tud_vendor_n_mounted(ITF_NUM_DAP))
        {
            tud_vendor_n_write_clear(ITF_NUM_DAP); // 断开：丢弃未发数据
            return;
        }
        const u32 n = tud_vendor_n_write(ITF_NUM_DAP, data + sent, len - sent);
        if (n > 0)
        {
            sent += static_cast<u16>(n);
            continue;
        }
        // FIFO 满（上一次 IN 未完成）：us 级忙等重试，最多 ~1ms；
        // 避免 vTaskDelay 的 tick 粒度（10ms@100Hz / 1ms@1000Hz）
        bool progressed = false;
        for (int w = 0; w < 10 && !progressed; w++)
        {
            esp_rom_delay_us(100);
            const u32 n2 = tud_vendor_n_write(ITF_NUM_DAP, data + sent, len - sent);
            if (n2 > 0)
            {
                sent += static_cast<u16>(n2);
                progressed = true;
            }
        }
        if (!progressed)
        {
            vTaskDelay(1); // 兜底：长时间排不空，让出 CPU
        }
    }
    // FIFO 模式（CFG_TUD_VENDOR_TXRX_BUFFERED=1）：写入字节数 < EP size 时
    // 不会自动发起 IN 传输，短响应必须显式 flush 才能送达主机
    tud_vendor_n_write_flush(ITF_NUM_DAP);
}

// 执行器命令源（USB 输入，dap_executor_run 的 cmd_src）：
// 从 bulk RX ringbuffer 取一条命令；Wireless 模式下不执行而是转发给
// 配对 peer 并等待响应写回 USB（返回 false = 无需执行）；
// Local 模式返回 true，由执行器执行并回传响应。
bool usb_cmd_src(u8 *buf, size_t buf_cap, u16 *len, void *arg)
{
    auto &usb = *reinterpret_cast<mlink::CmsisDapUsb *>(arg);

    size_t item_size;
    auto item = reinterpret_cast<u8 *>(xRingbufferReceive(usb.m_bulk_rx_buffer, &item_size, portMAX_DELAY));
    if (item == nullptr || item_size == 0)
    {
        return false;
    }
    if (item_size > buf_cap)
    {
        // EP 64B 下不可能发生；防御性丢弃
        ESP_LOGW(TAG, "Oversized DAP request (%u bytes), dropped", static_cast<unsigned>(item_size));
        vRingbufferReturnItem(usb.m_bulk_rx_buffer, item);
        return false;
    }
    std::memcpy(buf, item, item_size);
    vRingbufferReturnItem(usb.m_bulk_rx_buffer, item);
    *len = static_cast<u16>(item_size);

    if (!tud_vendor_n_mounted(ITF_NUM_DAP))
    {
        return false; // 主机未枚举完成前不处理
    }

    // 数据流向判断：Wireless 模式 → USB 收到的 DAP 命令转发给配对 peer
    // （完整无线调试数据通路）；Local 模式 → 交给执行器本地执行。
    // 具体发送/等待逻辑在 wireless_link 的转发接口里，这里只负责
    // 判断流向并把手里的 USB 响应写回。
    auto &app = mlink::Application::get_instance();
    if (app.mode() == mlink::Mode::Wireless)
    {
        u16 resp_len = 0;
        const esp_err_t ret =
            app.wireless_link().forward_dap_command(buf, *len, dap_response, sizeof(dap_response), &resp_len);
        if (ret == ESP_OK)
        {
            dap_send_response(dap_response, resp_len);
        }
        return false; // 无线路径已处理（或丢弃），无需执行
    }

    return true; // Local 模式：交给执行器执行
}

// 执行器响应宿（USB 输出，dap_executor_run 的 resp_sink）。
// 保持既有语义：超过 DAP_PACKET_SIZE 的响应（仅 0x7F 批量命令可触发）
// 截断发送，与上游 [DAP_PACKET_COUNT][DAP_PACKET_SIZE] 缓冲同语义。
void usb_resp_sink(const u8 *data, u16 len, [[maybe_unused]] void *arg)
{
    u16 out_len = len;
    if (out_len > DAP_PACKET_SIZE)
    {
        ESP_LOGW(TAG, "DAP response truncated (%u -> %u)", out_len, static_cast<unsigned>(DAP_PACKET_SIZE));
        out_len = DAP_PACKET_SIZE;
    }
    dap_send_response(data, out_len);
}

void dap_task(void *arg)
{
    // 复用公共执行器：命令源 = USB ringbuffer（Wireless 模式在源内转发），
    // 响应宿 = USB 写回；请求/响应缓冲用本文件传输层缓冲（不占任务栈）。
    // 执行循环、DAP_Setup 与命令日志均在 dap_executor.cc。
    mlink::dap_executor_run(usb_cmd_src, usb_resp_sink, arg, dap_request, sizeof(dap_request), dap_response,
                            sizeof(dap_response));
}

} // namespace

void tud_vendor_rx_cb(u8 idx, const u8 *buffer, u16 buffer_size)
{
    (void)buffer;
    (void)buffer_size;
    if (idx != ITF_NUM_DAP)
    {
        return;
    }

    // FIFO 模式（CFG_TUD_VENDOR_TXRX_BUFFERED=1）：回调不携带数据，
    // 必须从内部 RX FIFO 读取。tud_vendor_n_read 仅在该模式下可用。
    auto &usb = mlink::Application::get_instance().usb();
    u8 tmp[CONFIG_MEGUMINLINK_USB_MAX_PACKET_SIZE]; // 512B，≥ FIFO 64B
    u32 n;
    while ((n = tud_vendor_n_read(idx, tmp, sizeof(tmp))) > 0)
    {
        if (xRingbufferSend(usb.m_bulk_rx_buffer, tmp, n, pdMS_TO_TICKS(500)) != pdTRUE)
        {
            ESP_LOGW(TAG, "Ring buffer full, dropped %d bytes", (int)n);
            break;
        }
    }
}

const u8 *tud_descriptor_bos_cb()
{
    return BOS_DESCRIPTOR;
}

bool tud_vendor_control_xfer_cb(u8 rhport, u8 stage, const tusb_control_request_t *request)
{
    if (stage != CONTROL_STAGE_SETUP)
    {
        return true;
    }

    switch (request->bRequest)
    {
#if CONFIG_MEGUMINLINK_USB_WINUSB_CAPABILITY_ENABLE
    case VENDOR_CODE_MS_OS_20:
        if (request->wIndex == MS_OS_20_OP_GET_DESCRIPTOR_SET)
        {
            tud_control_xfer(rhport, request, MS_OS_20_DESCRIPTOR_SET, sizeof(MS_OS_20_DESCRIPTOR_SET));
            return true;
        }
        break;
#endif

#if CONFIG_MEGUMINLINK_USB_WEBUSB_CAPABILITY_ENABLE
    case VENDOR_CODE_WEBUSB:
        if (request->wIndex == WEBUSB_OP_GET_URL)
        {
            // URL Descriptor 在 flash 中只读，TinyUSB 传输路径按 void* 读取，
            // 去掉 const 仅是接口要求
            tud_control_xfer(rhport, request, const_cast<u8 *>(WEBUSB_URL_DESCRIPTOR.data()),
                             sizeof(WEBUSB_URL_DESCRIPTOR));
            return true;
        }
        break;
#endif
    }

    // 未识别的 vendor 请求：返回 false 让 usbd 对 EP0 发 STALL，
    // 避免主机干等超时
    return false;
}

namespace mlink
{
CmsisDapUsb::CmsisDapUsb()
{
    m_string_descriptors[(usize)UsbStringIndex::LangId] = LANGID;
    m_string_descriptors[(usize)UsbStringIndex::Manufacturer] = "MeguminLink";
    m_string_descriptors[(usize)UsbStringIndex::Product] = "CMSIS-DAP on " CONFIG_IDF_TARGET;
    m_string_descriptors[(usize)UsbStringIndex::SerialNumber] = m_serial;
    m_string_descriptors[(usize)UsbStringIndex::DapInterfaceName] = "CMSIS-DAP v2";

#if CONFIG_MEGUMINLINK_USB_WEBUSB_CAPABILITY_ENABLE
    m_string_descriptors[(usize)UsbStringIndex::WebUsbLandingPage] = WEBUSB_LANDING_PAGE.data();
#endif

    constexpr size_t RINGBUF_SIZE = CONFIG_MEGUMINLINK_USB_MAX_PACKET_SIZE * CONFIG_MEGUMINLINK_PACKET_BUFFER_DEPTH;

    m_bulk_rx_buffer = xRingbufferCreate(RINGBUF_SIZE, RINGBUF_TYPE_NOSPLIT);
}

CmsisDapUsb::~CmsisDapUsb()
{
    vRingbufferDelete(m_bulk_rx_buffer);
    m_bulk_rx_buffer = nullptr;

    vTaskDelete(m_dap_task);
    m_dap_task = nullptr;
}

esp_err_t CmsisDapUsb::init()
{
    if (m_initialized)
    {
        return ESP_OK;
    }

    esp_err_t ret;
    [[maybe_unused]] BaseType_t dap_core = 0; // 多核下 = TinyUSB 任务的反核；单核无意义

    {
        // 串号 = 芯片 MAC，保证多个 probe 可同时枚举
        u8 mac[6];
        ret = esp_read_mac(mac, ESP_MAC_EFUSE_FACTORY);
        ESP_RETURN_ON_ERROR(ret, TAG, "Failed to read MAC address");
        snprintf(m_serial, sizeof(m_serial), MACSTR, MAC2STR(mac));
    }

    {
        tinyusb_config_t tinyusb_config = TINYUSB_DEFAULT_CONFIG(tinyusb_event_callback);
        tinyusb_config.descriptor.device = &DEVICE_DESCRIPTOR;
        tinyusb_config.descriptor.string = m_string_descriptors.data();
        tinyusb_config.descriptor.string_count = static_cast<int>(m_string_descriptors.size());
        tinyusb_config.descriptor.full_speed_config = CONFIG_DESCRIPTOR;

#if !CONFIG_FREERTOS_UNICORE
        // 多核：显式固定 TinyUSB 任务到 core 1、优先级 5，不依赖库默认值
        // （TINYUSB_DEFAULT_TASK_AFFINITY/PRIO 可能随组件版本变化），确保
        // 后续创建的 DAP 任务可以确定性地与它分核。
        tinyusb_config.task.xCoreID = 1;
        tinyusb_config.task.priority = 5;
        dap_core = (tinyusb_config.task.xCoreID == 0) ? 1 : 0; // TinyUSB 反核
#endif

        ret = tinyusb_driver_install(&tinyusb_config);
        ESP_RETURN_ON_ERROR(ret, TAG, "Failed to initialize TinyUSB driver");
    }

#if CONFIG_MEGUMINLINK_USB_UART_BRIDGE_ENABLE
    ret = uart_bridge_init();
    ESP_RETURN_ON_ERROR(ret, TAG, "Failed to initialize USB CDC ACM interface");
#endif

    // 创建DAP处理任务。
    // 多核构建（如 ESP32-S3）：DAP 任务固定在 TinyUSB 的反核（上方显式配置
    // TinyUSB 在 core 1，故 DAP 任务在 core 0）且高优先级，保证 SWD 位时钟
    // 生成期间不被任务抢占（抢占会破坏时序、把实际 SWCLK 压到 ~130kHz）。
    // 单核构建（如 ESP32-C3/C2，CONFIG_FREERTOS_UNICORE）：无第二个核可
    // 用，若本任务高优先级会饿死同核的 TinyUSB 任务导致 USB 死锁，因此
    // 回退到与 TinyUSB 相同优先级（同优先级时间片轮转，互不饿死）。
#if CONFIG_FREERTOS_UNICORE
    xTaskCreate(dap_task, "CMSIS-DAP", 4096, this, 5, &m_dap_task);
#else
    xTaskCreatePinnedToCore(dap_task, "CMSIS-DAP", 4096, this, 20, &m_dap_task, dap_core);
#endif

    m_initialized = true;
    return ESP_OK;
}

void CmsisDapUsb::set_enabled(bool enabled)
{
    // tud_disconnect()/tud_connect() 线程安全（内部持有 usbd 互斥锁），
    // 可从任务上下文调用；tud_task 由 tinyusb_driver_install 创建后持续运行，
    // 断开/重连不会破坏驱动状态，重新连接时主机会重新枚举设备。
    if (enabled)
    {
        tud_connect();
        if (m_dap_task != nullptr)
        {
            vTaskResume(m_dap_task);
        }
#if CONFIG_MEGUMINLINK_USB_UART_BRIDGE_ENABLE
        if (m_uart_bridge_task != nullptr)
        {
            vTaskResume(m_uart_bridge_task);
        }
#endif
        ESP_LOGI(TAG, "USB enabled (local debug mode)");
    }
    else
    {
        // 先挂起数据处理任务，再断总线：suspend 时任务可能正阻塞在
        // ringbuffer/队列接收或忙等排空 TX，下个调度点即挂起，无并发风险。
        tud_disconnect();
        if (m_dap_task != nullptr)
        {
            vTaskSuspend(m_dap_task);
        }
#if CONFIG_MEGUMINLINK_USB_UART_BRIDGE_ENABLE
        if (m_uart_bridge_task != nullptr)
        {
            vTaskSuspend(m_uart_bridge_task);
        }
#endif
        ESP_LOGI(TAG, "USB disabled (wireless debug mode)");
    }
}
} // namespace mlink
