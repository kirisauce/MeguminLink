#include "meguminlink/usb/usb.hpp"
#include "meguminlink/app.hpp"
#include "meguminlink/dap_executor.hpp"  // dap_executor_run（Local 执行核心）
#include "meguminlink/usb/usb_capability.hpp"
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

// 注意 include 顺序：DAP_config.h 必须先于 DAP.h（与 cmsis_dap/Source/*.c 一致）。
// DAP_config.h 提供 DELAY_SLOW_CYCLES/PIN_DELAY_SLOW 等平台实现，
// DAP.h 仅在未定义时提供默认值（#ifndef 保护）；顺序反了会触发宏重定义。
// clang-format 的字典序排序会把它们排反，此处保持顺序：
// clang-format off
#include "DAP_config.h"
#include "DAP.h" // IWYU pragma: keep
// clang-format on

static constexpr char TAG[] = "mlink::usb";

/** 重新枚举前的断开时长：给主机足够时间感知拔出（软断开本身在 ms 级生效） */
static constexpr u32 RECONNECT_DELAY_MS = 100;

namespace
{
using namespace mlink;

// ---- CMSIS-DAP descriptors ----
// 接口结构：vendor specific 接口 (class 0xFF) + bulk 端点 承载 CMSIS-DAP v2
// 命令/响应；CONFIG_MEGUMINLINK_USB_UART_BRIDGE_ENABLE 开启时追加 IAD 组合的
// CDC ACM 接口作 UART 透传；CONFIG_MEGUMINLINK_USB_HID_DAP_ENABLE 开启时再
// 追加 HID 接口承载 CMSIS-DAP v1。两条 DAP 传输通道共用一条执行路径（见
// 「DAP 命令队列」段）。
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
// 接口结构（顺序即接口号；新接口一律追加在最后，保持既有接口号稳定）：
//   接口 0：vendor specific (0xFF, 0x00, 0x00) —— CMSIS-DAP v2 bulk transport
//     EP2 Bulk OUT（命令）+ EP3 Bulk IN（响应）
//   CONFIG_MEGUMINLINK_USB_UART_BRIDGE_ENABLE 开启时追加：
//   IAD 组合的 CDC ACM 串口（UART 透传）
//     EP4 Interrupt IN（通知）+ EP3 Bulk OUT / EP1 Bulk IN（数据）
//   CONFIG_MEGUMINLINK_USB_HID_DAP_ENABLE 开启时追加：
//   HID 接口（vendor-defined 64B 报告）—— CMSIS-DAP v1
//     EP1 Interrupt OUT（命令）+ EP2 Interrupt IN（响应）
enum
{
    ITF_NUM_DAP = 0,
#if CONFIG_MEGUMINLINK_USB_UART_BRIDGE_ENABLE
    ITF_NUM_CDC, // CDC ACM（通知接口）
    ITF_NUM_CDC_DATA,
#endif
#if CONFIG_MEGUMINLINK_USB_HID_DAP_ENABLE
    ITF_NUM_HID, // HID（CMSIS-DAP v1 传输）
#endif
    ITF_NUM_TOTAL,

    EP_DAP_OUT = 0x02, // Bulk OUT：命令
    EP_DAP_IN = 0x83,  // Bulk IN ：响应

#if CONFIG_MEGUMINLINK_USB_UART_BRIDGE_ENABLE
    EP_CDC_NOTIF = 0x84, // Interrupt IN：CDC 通知
    EP_CDC_OUT = 0x03,   // Bulk OUT：CDC 数据
    EP_CDC_IN = 0x81,    // Bulk IN ：CDC 数据
#endif

#if CONFIG_MEGUMINLINK_USB_HID_DAP_ENABLE
    EP_HID_OUT = 0x01, // Interrupt OUT：命令
    EP_HID_IN = 0x82,  // Interrupt IN ：响应
#endif
};

#if CONFIG_MEGUMINLINK_USB_HID_DAP_ENABLE
// ---- CMSIS-DAP v1 (HID) ----
// 报告描述符：vendor-defined usage page (0xFF00)、64B 定长 Input/Output 报告、
// 无 Report ID——与 DAPLink 参考实现一致。命令经 Output 报告（中断 OUT）下发、
// 响应经 Input 报告（中断 IN）回传，pyOCD/OpenOCD 的 HID 后端按定长 64B 收发。
constexpr u8 HID_REPORT_DESCRIPTOR[] = {
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

// HID 报告长度 = DAP_PACKET_SIZE（64）：CMSIS-DAP v1 的一条命令/响应占满一个
// 定长报告，短数据补零。
constexpr u16 HID_REPORT_SIZE = DAP_PACKET_SIZE;

// TinyUSB HID 类实例序号（≠ USB 接口号）：本固件只有一个 HID 接口
constexpr u8 HID_INSTANCE = 0;
#endif

// 配置描述符总长：接口组合取决于 UART 桥/HID 开关，用 consteval 求和，
// 避免为每种组合写一份 #if 分支。
consteval u16 config_descriptor_total_len()
{
    u16 len = TUD_CONFIG_DESC_LEN + TUD_VENDOR_DESC_LEN;

#if CONFIG_MEGUMINLINK_USB_UART_BRIDGE_ENABLE
    len += TUD_CDC_DESC_LEN;
#endif

#if CONFIG_MEGUMINLINK_USB_HID_DAP_ENABLE
    len += TUD_HID_INOUT_DESC_LEN;
#endif

    return len;
}

// bmAttributes = 0x80：bus powered
constexpr u8 CONFIG_DESCRIPTOR[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, config_descriptor_total_len(), 0x80, 500),

    TUD_VENDOR_DESCRIPTOR(ITF_NUM_DAP, (u8)UsbStringIndex::DapInterfaceName, EP_DAP_OUT, EP_DAP_IN, 64),
#if CONFIG_MEGUMINLINK_USB_UART_BRIDGE_ENABLE
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, (u8)UsbStringIndex::UartBridgeInterfaceName, EP_CDC_NOTIF, 64, EP_CDC_OUT,
                       EP_CDC_IN, 64),
#endif
#if CONFIG_MEGUMINLINK_USB_HID_DAP_ENABLE
    // 报告描述符长度 + 中断端点对（IN/OUT 各 64B，FS 轮询间隔 1ms）
    TUD_HID_INOUT_DESCRIPTOR(ITF_NUM_HID, (u8)UsbStringIndex::HidInterfaceName, HID_ITF_PROTOCOL_NONE,
                             sizeof(HID_REPORT_DESCRIPTOR), EP_HID_OUT, EP_HID_IN, HID_REPORT_SIZE, 1),
#endif

    // 如需 SWO 流，在此追加 EP3 Bulk In (0x83, 64B) 描述符
};

static_assert(sizeof(CONFIG_DESCRIPTOR) == config_descriptor_total_len());

#if CONFIG_MEGUMINLINK_USB_WEBUSB_CAPABILITY_ENABLE
// BOS WebUSB 平台能力里 iLandingPage 指向的字符串索引（字符串内容随 NVS 里的 URL 变）
constexpr u8 LANDING_PAGE_STRING_INDEX = static_cast<u8>(UsbStringIndex::WebUsbLandingPage);
#else
/** WebUSB 未编译时 BOS 里没有引用 landing page 的能力，索引取值无意义 */
constexpr u8 LANDING_PAGE_STRING_INDEX = 0;
#endif

// MS OS 2.0 描述符集圈定的接口号（usb_capability.cc）必须就是 DAP vendor 接口
static_assert(ITF_NUM_DAP == USB_CAPABILITY_INTERFACE);

void tinyusb_event_callback([[maybe_unused]] tinyusb_event_t *event, [[maybe_unused]] void *arg)
{
}

// ---- CMSIS-DAP 协议处理（v1 HID / v2 bulk 共用）----
// 统一由公共执行器 dap_executor_run（见 dap_executor.cc）驱动，本任务无独立
// 执行循环：两条 USB 传输通道（vendor bulk 与 HID 中断端点）收到的命令统一
// 入队，由同一个 dap_task 顺序执行——DAP_ExecuteCommand 带全局状态、不可重入，
// 两条通道的命令必须串行。队列元素自带来源标签，响应按来源写回对应通道；
// Wireless 模式在命令源内完成转发分流。

/** DAP 命令来源：决定响应写回哪条 USB 传输通道（见 dap_send_response） */
enum class DapSource : u8
{
    VendorBulk, // vendor 接口 bulk 端点（CMSIS-DAP v2）
    Hid,        // HID 接口中断端点（CMSIS-DAP v1）
};

struct DapCommand
{
    DapSource source;
    u16 len;
    u8 data[DAP_PACKET_SIZE];
};

// 请求/响应缓冲为 USB 传输层资源（不占任务栈——ESP-IDF 任务栈以字节计，
// DAP 执行调用链栈需求大，缓冲放栈上会溢出）。
u8 dap_request[DAP_PACKET_SIZE];
// 响应缓冲按 CMSIS-DAP 批量响应上限预留（DAP_MAX_RESPONSE_SIZE，见
// dap_executor.hpp）：Local 模式单命令响应 ≤ DAP_PACKET_SIZE，0x7F 批量
// 响应可到 ~2KB（DAP_ExecuteCommand 批量写入不检查边界）；Wireless 模式
// 等待到的无线响应（≤ MAX_DAP_PAYLOAD_SIZE）同样拷入此缓冲再写回 USB。
u8 dap_response[mlink::DAP_MAX_RESPONSE_SIZE];

// 命令入队。调用方为 TinyUSB 任务上下文（vendor bulk RX 回调 / HID SET_REPORT
// 回调），两者共用一条队列，元素自带来源标签。队列满时丢弃并告警：PC 端
// debugger 等不到响应会自行超时重试。
void enqueue_dap_command(DapSource source, const u8 *data, u16 len)
{
    if (len == 0)
    {
        return;
    }
    if (len > DAP_PACKET_SIZE)
    {
        // EP 64B 下不可能发生；防御性截断
        ESP_LOGW(TAG, "Oversized DAP command (%u bytes), truncated", static_cast<unsigned>(len));
        len = DAP_PACKET_SIZE;
    }

    DapCommand cmd{};
    cmd.source = source;
    cmd.len = len;
    std::memcpy(cmd.data, data, len);

    auto &usb = mlink::Application::get_instance().usb();
    if (xRingbufferSend(usb.m_dap_command_buffer, &cmd, sizeof(cmd), pdMS_TO_TICKS(500)) != pdTRUE)
    {
        ESP_LOGW(TAG, "DAP command queue full, dropped %u bytes", static_cast<unsigned>(len));
    }
}

// 响应写入 vendor TX FIFO：write 同步拷贝入 FIFO，满时返回 0；
// tinyusb_task 后台排空。拷贝完成即缓冲区可复用，无需等发送完毕。
// FIFO 满的根因：上一次 IN 传输的完成回调（TinyUSB 任务处理）相对本任务
// 滞后，endpoint 仍 busy，数据留在 FIFO。此时 vTaskDelay(1) 的粒度是
// 1 个 tick（此前 100Hz = 10ms！），每次响应都白等一个 tick，读吞吐被压到
// ~5KB/s。改用 us 级忙等重试，只有长时间排不空才兜底让出。
void dap_send_vendor_response(const u8 *data, u16 len)
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

#if CONFIG_MEGUMINLINK_USB_HID_DAP_ENABLE
// 响应经 HID 中断 IN 端点下发：CMSIS-DAP v1 的响应占满一个定长 64B 报告
// （不足补零），超长按 DAP_PACKET_SIZE 截断——与 v2 bulk 路径同语义。
void dap_send_hid_response(const u8 *data, u16 len)
{
    u16 out_len = len;
    if (out_len > HID_REPORT_SIZE)
    {
        ESP_LOGW(TAG, "HID response truncated (%u -> %u)", static_cast<unsigned>(out_len),
                 static_cast<unsigned>(HID_REPORT_SIZE));
        out_len = HID_REPORT_SIZE;
    }

    u8 report[HID_REPORT_SIZE] = {};
    std::memcpy(report, data, out_len);

    // 响应必须送达：主机按命令顺序逐个读 HID 报告（不像 v2 bulk 那样有独立
    // 数据流可供重新同步），丢一条会让其后的响应与命令整体错位。IN 端点忙
    // （上一次报告尚未被主机取走）时让出重试，直到发出或设备被关闭。
    while (tud_ready())
    {
        if (tud_hid_n_report(HID_INSTANCE, 0, report, HID_REPORT_SIZE))
        {
            return;
        }
        vTaskDelay(1);
    }
}
#endif

// 响应写回命令来源对应的传输通道
void dap_send_response(DapSource source, const u8 *data, u16 len)
{
#if CONFIG_MEGUMINLINK_USB_HID_DAP_ENABLE
    if (source == DapSource::Hid)
    {
        dap_send_hid_response(data, len);
        return;
    }
#endif
    dap_send_vendor_response(data, len);
}

// 命令来源通道是否可用：不可用时丢弃命令。
// vendor bulk 侧用 mounted 判断（主机未枚举完成前不处理残留命令）。
// HID 侧无条件放行——命令本身就是从 HID 的 SET_REPORT 回调进来的，通道必然
// 可用；这里绝不能用 tud_hid_n_ready()：它把"上一条 IN 报告尚未被主机取走"
// 也算作不可用，而主机会按 DAP_Info 上报的 packet count 流水线多条命令
// （OpenOCD 会连发最多 3 条），此时丢弃命令会让其后的响应与命令整体错位
// ——实测症状为 OpenOCD 报 "CMSIS-DAP transfer count mismatch"。
bool dap_source_ready(DapSource source)
{
#if CONFIG_MEGUMINLINK_USB_HID_DAP_ENABLE
    if (source == DapSource::Hid)
    {
        return true;
    }
#endif
    return tud_vendor_n_mounted(ITF_NUM_DAP);
}

// 执行器上下文：命令队列、当前在途命令的来源（响应路由依据）与请求长度
// （统计用）。cmd_src 与 resp_sink 由同一任务（dap_task）串行调用，无需同步。
struct DapTaskContext
{
    mlink::CmsisDapUsb *usb;
    DapSource source = DapSource::VendorBulk;
    u16 request_len = 0;
};

// 执行器命令源（USB 输入，dap_executor_run 的 cmd_src）：
// 从命令队列取一条命令（两条传输通道共用）；Wireless 模式下不执行而是转发给
// 配对 peer 并按来源写回 USB（返回 false = 无需执行）；
// Local 模式返回 true，由执行器执行并回传响应。
bool usb_cmd_src(u8 *buf, size_t buf_cap, u16 *len, void *arg)
{
    auto &ctx = *static_cast<DapTaskContext *>(arg);

    size_t item_size = 0;
    auto item =
        reinterpret_cast<DapCommand *>(xRingbufferReceive(ctx.usb->m_dap_command_buffer, &item_size, portMAX_DELAY));
    if (item == nullptr || item_size != sizeof(DapCommand))
    {
        // 队列被写坏（长度不符）：丢弃该项，避免按错误布局解读
        if (item != nullptr)
        {
            vRingbufferReturnItem(ctx.usb->m_dap_command_buffer, item);
        }
        return false;
    }

    const DapSource source = item->source;
    const u16 req_len = (item->len <= buf_cap) ? item->len : static_cast<u16>(buf_cap);
    std::memcpy(buf, item->data, req_len);
    vRingbufferReturnItem(ctx.usb->m_dap_command_buffer, item);
    *len = req_len;
    ctx.source = source;
    ctx.request_len = req_len;

    if (!dap_source_ready(source))
    {
        return false; // 通道不可用（主机未枚举完成）：丢弃
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
            dap_send_response(source, dap_response, resp_len);
        }
        return false; // 无线路径已处理（或丢弃），无需执行
    }

    return true; // Local 模式：交给执行器执行
}

// 执行器响应宿（USB 输出，dap_executor_run 的 resp_sink）。
// 保持既有语义：超过 DAP_PACKET_SIZE 的响应（仅 0x7F 批量命令可触发）
// 截断发送，与上游 [DAP_PACKET_COUNT][DAP_PACKET_SIZE] 缓冲同语义。
void usb_resp_sink(const u8 *data, u16 len, void *arg)
{
    auto &ctx = *static_cast<DapTaskContext *>(arg);

    u16 out_len = len;
    if (out_len > DAP_PACKET_SIZE)
    {
        ESP_LOGW(TAG, "DAP response truncated (%u -> %u)", out_len, static_cast<unsigned>(DAP_PACKET_SIZE));
        out_len = DAP_PACKET_SIZE;
    }
    dap_send_response(ctx.source, data, out_len);
    Application::get_instance().config_channel().add_dap_command(ctx.request_len, out_len);
}

void dap_task(void *arg)
{
    // 复用公共执行器：命令源 = USB 命令队列（两条传输通道共用；Wireless 模式
    // 在源内转发），响应宿 = 按来源写回对应通道；请求/响应缓冲用本文件传输层
    // 缓冲（不占任务栈）。执行循环、DAP_Setup 与命令日志均在 dap_executor.cc。
    DapTaskContext ctx{reinterpret_cast<mlink::CmsisDapUsb *>(arg)};
    mlink::dap_executor_run(usb_cmd_src, usb_resp_sink, &ctx, dap_request, sizeof(dap_request), dap_response,
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
    // 每块数据即一条 DAP 命令（EP 64B = DAP_PACKET_SIZE）：入队后由 dap_task
    // 与 HID 通道的命令一起顺序执行。
    u8 tmp[CONFIG_MEGUMINLINK_USB_MAX_PACKET_SIZE]; // 512B，≥ FIFO 64B
    u32 n;
    while ((n = tud_vendor_n_read(idx, tmp, sizeof(tmp))) > 0)
    {
        enqueue_dap_command(DapSource::VendorBulk, tmp, static_cast<u16>(n));
    }
}

// HID 类回调：TinyUSB 只对 report_complete/protocol/idle 提供弱默认实现，
// 这三个是硬链接依赖，TINYUSB_HID_COUNT>0 时必须由应用提供（HID 接口关闭时
// 同样要定义——HID 类代码已编入固件，只是没有接口引用它）。
#if CONFIG_TINYUSB_HID_COUNT > 0
u8 const *tud_hid_descriptor_report_cb([[maybe_unused]] u8 instance)
{
#if CONFIG_MEGUMINLINK_USB_HID_DAP_ENABLE
    return HID_REPORT_DESCRIPTOR;
#else
    // HID 接口未暴露（MEGUMINLINK_USB_HID_DAP_ENABLE=n）：描述符里没有 HID
    // 接口，本回调不会被调用。
    return nullptr;
#endif
}

u16 tud_hid_get_report_cb([[maybe_unused]] u8 instance, [[maybe_unused]] u8 report_id,
                          [[maybe_unused]] hid_report_type_t report_type, [[maybe_unused]] u8 *buffer,
                          [[maybe_unused]] u16 reqlen)
{
    // 响应只经中断 IN 报告下发（CMSIS-DAP v1 传输方式）：不缓存响应，
    // GET_REPORT 无数据可回。
    return 0;
}

void tud_hid_set_report_cb([[maybe_unused]] u8 instance, [[maybe_unused]] u8 report_id,
                           [[maybe_unused]] hid_report_type_t report_type, u8 const *buffer, u16 bufsize)
{
#if CONFIG_MEGUMINLINK_USB_HID_DAP_ENABLE
    // DAP v1：一条命令封装在一个定长 Output 报告里。中断 OUT 端点与控制传输
    // SET_REPORT 两条路径都进这里，统一入队交给 dap_task。
    if (instance != HID_INSTANCE || report_type != HID_REPORT_TYPE_OUTPUT || bufsize == 0)
    {
        return;
    }
    enqueue_dap_command(DapSource::Hid, buffer, bufsize);
#endif
}
#endif // CONFIG_TINYUSB_HID_COUNT > 0

const u8 *tud_descriptor_bos_cb()
{
    // BOS 按运行时的能力状态构造（见 usb_capability.cc）：主机只在枚举时读取，
    // 运行时开关变化靠 apply_usb_capability 的重新枚举让它重读
    return mlink::Application::get_instance().usb().usb_descriptors().bos();
}

bool tud_vendor_control_xfer_cb(u8 rhport, u8 stage, const tusb_control_request_t *request)
{
    // 配置通道（bRequest = 0x4D）：EP0 厂商请求，与下述 MS OS 2.0 / WebUSB
    // 的描述符请求按 bRequest 分派；非本模块的请求交回下方原逻辑处理
    if (mlink::Application::get_instance().config_channel().handle_control(rhport, stage, request))
    {
        return true;
    }

    if (stage != CONTROL_STAGE_SETUP)
    {
        return true;
    }

    // 运行时被关掉的能力不再响应其厂商请求（STALL）：主机枚举时已从 BOS 得知
    // 设备不宣告该能力，正常不会发这些请求过来
    const mlink::CmsisDapUsb &usb = mlink::Application::get_instance().usb();

    switch (request->bRequest)
    {
#if CONFIG_MEGUMINLINK_USB_WINUSB_CAPABILITY_ENABLE
    case VENDOR_CODE_MS_OS_20:
        if (request->wIndex == MS_OS_20_OP_GET_DESCRIPTOR_SET && usb.usb_capability().winusb != 0)
        {
            tud_control_xfer(rhport, request, const_cast<u8 *>(usb.usb_descriptors().ms_os_descriptor_set()),
                             usb.usb_descriptors().ms_os_descriptor_set_len());
            return true;
        }
        break;
#endif

#if CONFIG_MEGUMINLINK_USB_WEBUSB_CAPABILITY_ENABLE
    case VENDOR_CODE_WEBUSB:
        if (request->wIndex == WEBUSB_OP_GET_URL && usb.usb_capability().webusb != 0)
        {
            // URL Descriptor 常驻内存，TinyUSB 传输路径按 void* 读取，
            // 去掉 const 仅是接口要求
            tud_control_xfer(rhport, request, const_cast<u8 *>(usb.usb_descriptors().url_descriptor()),
                             usb.usb_descriptors().url_descriptor_len());
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
#if CONFIG_MEGUMINLINK_USB_HID_DAP_ENABLE
    m_string_descriptors[(usize)UsbStringIndex::HidInterfaceName] = "CMSIS-DAP v1";
#endif

    // 能力状态从 NVS 载入（无记录用编译期默认值），并据此构造 BOS 与 GET_URL 响应：
    // 必须在 tinyusb_driver_install（init）之前完成，主机枚举时读到的就是它
    m_usb_capability = usb_capability_load();
#if CONFIG_MEGUMINLINK_USB_WEBUSB_CAPABILITY_ENABLE
    m_string_descriptors[(usize)UsbStringIndex::WebUsbLandingPage] = m_usb_capability.landing_page;
#endif
    m_usb_descriptors.rebuild(m_usb_capability, LANDING_PAGE_STRING_INDEX);

    // 命令队列按字节容量创建：每条命令占一个 DapCommand（4 + DAP_PACKET_SIZE 字节），
    // 默认配置下可积压数十条，远大于 DAP_PACKET_COUNT 的流水线深度。
    constexpr size_t RINGBUF_SIZE = CONFIG_MEGUMINLINK_USB_MAX_PACKET_SIZE * CONFIG_MEGUMINLINK_PACKET_BUFFER_DEPTH;

    m_dap_command_buffer = xRingbufferCreate(RINGBUF_SIZE, RINGBUF_TYPE_NOSPLIT);
}

CmsisDapUsb::~CmsisDapUsb()
{
    vRingbufferDelete(m_dap_command_buffer);
    m_dap_command_buffer = nullptr;

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

void CmsisDapUsb::apply_usb_capability(const UsbCapabilitySettings &settings)
{
    // 断开窗口内重建：主机收到软断开后停止请求描述符，缓冲与 iLandingPage 指向的
    // 字符串内容不会被 TinyUSB 任务并发读取；重建完成再连接，主机重新枚举并重读
    tud_disconnect();
    vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MS));

    m_usb_capability = settings;
    m_usb_descriptors.rebuild(m_usb_capability, LANDING_PAGE_STRING_INDEX);
#if CONFIG_MEGUMINLINK_USB_WEBUSB_CAPABILITY_ENABLE
    m_string_descriptors[(usize)UsbStringIndex::WebUsbLandingPage] = m_usb_capability.landing_page;
#endif

    tud_connect();
    ESP_LOGI(TAG, "USB capabilities applied (webusb=%u winusb=%u landing_page=%s), re-enumerating",
             static_cast<unsigned>(m_usb_capability.webusb), static_cast<unsigned>(m_usb_capability.winusb),
             m_usb_capability.landing_page);
}
} // namespace mlink
