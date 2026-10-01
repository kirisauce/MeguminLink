#include "meguminlink/usb/usb.hpp"
#include "meguminlink/app.hpp"
#include "meguminlink/dap_executor.hpp"  // dap_executor_run（Local 执行核心）
#include "meguminlink/dap_frame.hpp"     // dap_request_size（USB 命令重组）
#include "meguminlink/usb/update_mode.hpp" // UpdateMode::try_instance（更新模式分流）
#include "meguminlink/usb/usb_capability.hpp"
#include "meguminlink/usb/usb_descriptors.hpp"
#include "meguminlink/wireless/frame.hpp"
#include "meguminlink/wireless/link.hpp" // WirelessLink::submit_dap_command（Wireless 转发）

#include <esp_check.h>
#include <esp_log.h>
#include <esp_rom_sys.h> // esp_rom_delay_us
#include <sdkconfig.h>
#include <tinyusb.h>
#include <tinyusb_default_config.h>
#include <tusb.h>

#include <cstddef>
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

/**
 * vendor IN 已发出的累计字节数（由 tud_vendor_tx_cb 累加）。
 *
 * 响应写回靠它确认"这一整条真的被主机读走了"：vendor FIFO 是字节流、没有消息边界，
 * 两条响应同时排在里面会被主机的一次读合并成一条，而按"一次读一条响应"解析的主机
 * （pyocd 的延迟传输正是如此）只能解出第一条、其余全丢。注意一条响应可能跨多次 IN
 * 传输（端点 64B），所以判据是"累计字节数够了"，而不是"回调来过一次"。
 */
static volatile u32 s_vendor_tx_bytes = 0;

/** 重新枚举前的断开时长：给主机足够时间感知拔出（软断开本身在 ms 级生效） */
static constexpr u32 RECONNECT_DELAY_MS = 100;

namespace
{
using namespace mlink;

#if CONFIG_MEGUMINLINK_USB_HID_DAP_ENABLE
// TinyUSB HID 类实例序号（≠ USB 接口号）：本固件只有一个 HID 接口
constexpr u8 HID_INSTANCE = 0;

// 描述符给出的 HID 报告长度必须等于 CMSIS-DAP 的单包尺寸：v1 路径按定长报告收发
static_assert(USB_HID_REPORT_SIZE == DAP_PACKET_SIZE, "HID report size must match CMSIS-DAP packet size");
#endif

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
    u8 data[MAX_DAP_PAYLOAD_SIZE];
};

// 请求/响应缓冲为 USB 传输层资源（不占任务栈——ESP-IDF 任务栈以字节计，
// DAP 执行调用链栈需求大，缓冲放栈上会溢出）。
u8 dap_request[MAX_DAP_PAYLOAD_SIZE];
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
    if (len > MAX_DAP_PAYLOAD_SIZE)
    {
        ESP_LOGW(TAG, "Oversized DAP command (%u bytes), dropped", static_cast<unsigned>(len));
        return;
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

// 跨 USB 包累积一条完整 DAP 命令。命令可能大于单个 USB 包（EP 64B），
// 接收回调每次只拿到一段，必须按命令自描述长度重组后再入队；一次回调也
// 可能带回多条命令。仅 TinyUSB 任务上下文读写，缓冲随 USB 对象持有。
void accumulate_dap_command(const u8 *data, size_t len)
{
    auto &usb = mlink::Application::get_instance().usb();
    u8 *const accum = usb.m_rx_accum;
    size_t &accum_len = usb.m_rx_accum_len;

    while (len > 0)
    {
        if (accum_len == sizeof(usb.m_rx_accum))
        {
            // 缓冲已满仍拼不出完整命令：长度非法或超出单帧承载上限
            ESP_LOGW(TAG, "DAP command exceeds %u bytes, dropped", static_cast<unsigned>(sizeof(usb.m_rx_accum)));
            accum_len = 0;
        }

        const size_t space = sizeof(usb.m_rx_accum) - accum_len;
        const size_t take = (len < space) ? len : space;
        std::memcpy(accum + accum_len, data, take);
        accum_len += take;
        data += take;
        len -= take;

        const size_t cmd_len = mlink::dap_request_size(accum, accum_len);
        if (cmd_len == 0)
        {
            continue; // 数据不足：等后续包补齐
        }

        enqueue_dap_command(DapSource::VendorBulk, accum, static_cast<u16>(cmd_len));

        accum_len -= cmd_len;
        if (accum_len > 0)
        {
            std::memmove(accum, accum + cmd_len, accum_len);
        }
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
        if (!tud_vendor_n_mounted(USB_DAP_INTERFACE))
        {
            tud_vendor_n_write_clear(USB_DAP_INTERFACE); // 断开：丢弃未发数据
            return;
        }
        const u32 n = tud_vendor_n_write(USB_DAP_INTERFACE, data + sent, len - sent);
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
            const u32 n2 = tud_vendor_n_write(USB_DAP_INTERFACE, data + sent, len - sent);
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
    tud_vendor_n_write_flush(USB_DAP_INTERFACE);

    // 等这条响应整条被主机读走再返回（见 s_vendor_tx_bytes 的说明）：端点只有 64B，
    // 大响应要跨多次 IN 传输，只等第一段就放行会把这条响应截断、下一条接上去。
    // 等待要够长：主机在连发命令时不会同时读响应。500ms 仍等不到就交给超时销账兜底。
    const u32 sent_before = s_vendor_tx_bytes;
    for (int w = 0; w < 500; w++)
    {
        if (!tud_vendor_n_mounted(USB_DAP_INTERFACE))
        {
            return;
        }
        if ((s_vendor_tx_bytes - sent_before) >= len)
        {
            break;
        }
        vTaskDelay(1);
    }
}

#if CONFIG_MEGUMINLINK_USB_HID_DAP_ENABLE
// 响应经 HID 中断 IN 端点下发：CMSIS-DAP v1 的响应占满一个定长 64B 报告
// （不足补零），超长按 DAP_PACKET_SIZE 截断——与 v2 bulk 路径同语义。
void dap_send_hid_response(const u8 *data, u16 len)
{
    u16 out_len = len;
    if (out_len > USB_HID_REPORT_SIZE)
    {
        ESP_LOGW(TAG, "HID response truncated (%u -> %u)", static_cast<unsigned>(out_len),
                 static_cast<unsigned>(USB_HID_REPORT_SIZE));
        out_len = USB_HID_REPORT_SIZE;
    }

    u8 report[USB_HID_REPORT_SIZE] = {};
    std::memcpy(report, data, out_len);

    // 响应必须送达：主机按命令顺序逐个读 HID 报告（不像 v2 bulk 那样有独立
    // 数据流可供重新同步），丢一条会让其后的响应与命令整体错位。IN 端点忙
    // （上一次报告尚未被主机取走）时让出重试，直到发出或设备被关闭。
    // 退出条件必须带 hid_exposed()：接口不暴露时 tud_hid_n_report 恒失败，
    // 只判 tud_ready() 会空转。
    const CmsisDapUsb &usb = Application::get_instance().usb();
    while (tud_ready() && usb.hid_exposed())
    {
        if (tud_hid_n_report(HID_INSTANCE, 0, report, USB_HID_REPORT_SIZE))
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
// HID 侧只认兼容模式位：命令本来就是从 HID 的 SET_REPORT 回调进来的，兼容模式下
// 通道必然可用；关闭兼容模式后队列里的残留命令只能丢弃（主机那条命令自己超时
// 重试）。这里绝不能用 tud_hid_n_ready()：它把"上一条 IN 报告尚未被主机取走"
// 也算作不可用，而主机会按 DAP_Info 上报的 packet count 流水线多条命令
// （OpenOCD 会连发最多 3 条），此时丢弃命令会让其后的响应与命令整体错位
// ——实测症状为 OpenOCD 报 "CMSIS-DAP transfer count mismatch"。
bool dap_source_ready(DapSource source)
{
#if CONFIG_MEGUMINLINK_USB_HID_DAP_ENABLE
    if (source == DapSource::Hid)
    {
        return Application::get_instance().usb().hid_exposed();
    }
#endif
    return tud_vendor_n_mounted(USB_DAP_INTERFACE);
}

/** 一条已提交、还没拿到响应的无线 DAP 命令（响应按可靠传输序号配回来） */
struct PendingWirelessCommand
{
    bool used = false;
    u16 seq = 0;
    /** 命令号（请求首字节）：销账日志用它指明失败的是哪条命令 */
    u8 cmd_id = 0;
    DapSource source = DapSource::VendorBulk;
    u16 request_len = 0;
    TickType_t sent_tick = 0;
};

// 执行器上下文：命令队列、当前在途命令的来源（响应路由依据）与请求长度（统计用）。
// 无线模式下还持有"已提交未响应"的挂起表：命令发出后立刻回来取下一条，响应到达
// 时按序号配回，多条命令因此可以同时在途（窗口才有意义）。cmd_src 与 resp_sink
// 由同一任务（dap_task）串行调用，挂起表无需同步。
struct DapTaskContext
{
    mlink::CmsisDapUsb *usb;
    DapSource source = DapSource::VendorBulk;
    u16 request_len = 0;
    PendingWirelessCommand pending[CONFIG_MEGUMINLINK_PACKET_BUFFER_DEPTH];
};

/**
 * 取最老的一条挂起条目（响应按先进先出配回）。
 *
 * 不能用响应携带的 seq 去匹配命令：两端各有独立的发送序号空间，响应上的 seq 是
 * 对端发那条响应时用的序号，与本地命令的序号没有对应关系。窗口为 1 时两侧恰好
 * 同步递增，看着能对上；一旦并发、或任一端重置过会话，就会整体错位——响应全部
 * 配不上而被当成迟到丢掉，主机侧表现为收不到响应直至超时。
 *
 * 可靠传输保证响应按命令顺序交付（乱序已在接收侧重排），所以顺序配对是成立的。
 */
PendingWirelessCommand *oldest_pending(DapTaskContext &ctx)
{
    PendingWirelessCommand *found = nullptr;
    for (PendingWirelessCommand &pending : ctx.pending)
    {
        if (!pending.used)
        {
            continue;
        }
        // tick 回绕周期远长于挂起条目的存活时间（< 2s），直接比大小即可
        if (found == nullptr || pending.sent_tick < found->sent_tick)
        {
            found = &pending;
        }
    }
    return found;
}

/** 按命令自己的序号找挂起条目（失败销账用：take_failed 给的是本地命令序号） */
PendingWirelessCommand *find_pending(DapTaskContext &ctx, u16 seq)
{
    for (PendingWirelessCommand &pending : ctx.pending)
    {
        if (pending.used && pending.seq == seq)
        {
            return &pending;
        }
    }
    return nullptr;
}

/** 提交一条无线 DAP 命令并登记在途条目；提交失败不占槽（主机侧自行重试） */
void submit_wireless_command(DapTaskContext &ctx, const u8 *cmd, u16 len)
{
    auto &app = mlink::Application::get_instance();
    const u8 cmd_id = (len > 0) ? cmd[0] : 0;

    PendingWirelessCommand *slot = nullptr;
    for (PendingWirelessCommand &pending : ctx.pending)
    {
        if (!pending.used)
        {
            slot = &pending;
            break;
        }
    }
    if (slot == nullptr)
    {
        ESP_LOGW(TAG, "no free in-flight slot, DAP cmd 0x%02X (%s) len=%u dropped", cmd_id, dap_command_name(cmd_id),
                 static_cast<unsigned>(len));
        return;
    }

    u16 seq = 0;
    // 通道标记随命令送到对端：对端据此回答 DAP_Info 的单包尺寸，HID 与 bulk 的
    // 可承载命令大小不同（见 DAP_CHANNEL_*）
    const u8 channel = (ctx.source == DapSource::Hid) ? DAP_CHANNEL_HID : DAP_CHANNEL_VENDOR_BULK;
    const esp_err_t ret = app.wireless_link().submit_dap_command(cmd, len, channel, &seq);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "cannot submit DAP cmd 0x%02X (%s) len=%u: %s", cmd_id, dap_command_name(cmd_id),
                 static_cast<unsigned>(len), esp_err_to_name(ret));
        return;
    }

    slot->used = true;
    slot->seq = seq;
    slot->cmd_id = cmd_id;
    slot->source = ctx.source;
    slot->request_len = len;
    slot->sent_tick = xTaskGetTickCount();
}

/**
 * 无线模式的响应回收：把已到达的响应按序号配回挂起命令并写回 USB，同时给失败与
 * 超时的命令销账。
 *
 * 销账策略是不回响应：CMSIS-DAP 没有通用的失败包，伪造形状难保被主机接受；只把
 * 命令从在途表摘掉并打日志，让主机那条命令自己超时重试。迟到的响应同样丢弃——
 * 回写它会让主机后续的响应整体错位。
 */
void drain_wireless_responses(DapTaskContext &ctx)
{
    auto &app = mlink::Application::get_instance();

    // 1) 已到达的响应：按序号配回，并按命令当初的来源通道写回
    for (;;)
    {
        u16 seq = 0;
        u16 len = 0;
        if (!app.wait_wireless_response(&seq, dap_response, &len, 0))
        {
            break;
        }

        PendingWirelessCommand *pending = oldest_pending(ctx);
        if (pending == nullptr)
        {
            // 没有任何在途命令：已销账的命令迟到的响应，回写会让主机响应流错位
            ESP_LOGW(TAG, "late wireless DAP response seq=%u (%u bytes) dropped", seq, static_cast<unsigned>(len));
            continue;
        }

        dap_send_response(pending->source, dap_response, len);
        app.config_channel().add_dap_command(pending->request_len, len);
        pending->used = false;
    }

    // 2) 可靠传输已判定失败的命令（重传耗尽）
    u16 failed_seq = 0;
    while (app.wireless_link().take_failed_command(&failed_seq))
    {
        PendingWirelessCommand *pending = find_pending(ctx, failed_seq);
        if (pending == nullptr)
        {
            continue;
        }
        ESP_LOGW(TAG, "wireless DAP command seq=%u cmd=0x%02X (%s) len=%u failed after all retries, no response sent",
                 failed_seq, pending->cmd_id, dap_command_name(pending->cmd_id),
                 static_cast<unsigned>(pending->request_len));
        pending->used = false;
    }

    // 3) 硬超时兜底：可靠传输的失败判定比它早，未启用可靠传输时只有这一道
    const TickType_t now = xTaskGetTickCount();
    for (PendingWirelessCommand &pending : ctx.pending)
    {
        if (!pending.used)
        {
            continue;
        }
        const u32 waited_ms = static_cast<u32>((now - pending.sent_tick) * portTICK_PERIOD_MS);
        if (waited_ms < CONFIG_MEGUMINLINK_WIRELESS_RESPONSE_TIMEOUT_MS)
        {
            continue;
        }
        ESP_LOGW(TAG, "wireless DAP command seq=%u cmd=0x%02X (%s) len=%u timed out after %u ms, no response sent",
                 pending.seq, pending.cmd_id, dap_command_name(pending.cmd_id),
                 static_cast<unsigned>(pending.request_len), static_cast<unsigned>(waited_ms));
        pending.used = false;
    }
}

// 执行器命令源（USB 输入，dap_executor_run 的 cmd_src）：
// 从命令队列取一条命令（两条传输通道共用）；Wireless 模式下不执行而是提交给配对
// peer 后立刻回来取下一条，响应由 drain_wireless_responses 配回并写回 USB
// （返回 false = 无需执行）；Local 模式返回 true，由执行器执行并回传响应。
bool usb_cmd_src(u8 *buf, size_t buf_cap, u16 *len, void *arg)
{
    auto &ctx = *static_cast<DapTaskContext *>(arg);
    auto &app = mlink::Application::get_instance();
    const bool wireless = (app.mode() == mlink::Mode::Wireless);

    // 可靠传输窗口没有空位时先不取命令：让它留在队列里形成背压（队列满 → 主机 USB
    // 写不进来自然等），而不是取出来丢弃——丢弃会让主机那条命令永远等不到响应。
    // 排空响应会释放窗口，所以这里醒来一次就够，1ms 的让出避免空转。
    if (wireless && !app.wireless_link().can_submit_command())
    {
        drain_wireless_responses(ctx);
        vTaskDelay(1);
        return false;
    }

    // 无线模式下响应要尽快回写：没有命令可取的间隙也得醒来排空一次。1ms 的取用
    // 超时让响应最多晚 1ms 回写，而 tick 本身就是 1ms，额外开销可忽略。
    size_t item_size = 0;
    auto item = reinterpret_cast<DapCommand *>(
        xRingbufferReceive(ctx.usb->m_dap_command_buffer, &item_size, wireless ? pdMS_TO_TICKS(1) : portMAX_DELAY));
    if (item == nullptr || item_size != sizeof(DapCommand))
    {
        // 队列被写坏（长度不符）：丢弃该项，避免按错误布局解读
        if (item != nullptr)
        {
            vRingbufferReturnItem(ctx.usb->m_dap_command_buffer, item);
        }
        if (wireless)
        {
            drain_wireless_responses(ctx);
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

    // DAP_Info 上报的单包尺寸按传输通道设定：vendor bulk 受 CMSIS-DAP 单字节
    // Transfer Count 的安全上限约束（见 DAP_REPORTED_PACKET_SIZE_MAX），HID 是
    // 定长 64B 报告。主机据此决定一条命令能携带多少数据。
    DAP_Data.packet_size = (source == DapSource::VendorBulk)
                               ? static_cast<u16>(DAP_REPORTED_PACKET_SIZE_MAX)
                               : DAP_HID_PACKET_SIZE;

    if (!dap_source_ready(source))
    {
        return false; // 通道不可用（主机未枚举完成）：丢弃
    }

    // 数据流向判断：Wireless 模式 → 提交给配对 peer（完整无线调试数据通路）；
    // Local 模式 → 交给执行器本地执行。
    if (wireless)
    {
        // 先把已到达的响应写回（顺序即命令顺序），再提交本条命令
        drain_wireless_responses(ctx);
        submit_wireless_command(ctx, buf, *len);
        return false; // 无线路径已处理（或丢弃），无需执行
    }

    return true; // Local 模式：交给执行器执行
}

// 执行器响应宿（USB 输出，dap_executor_run 的 resp_sink）。
// 响应按来源通道原样下发，不在本层截断：HID 通道由 dap_send_hid_response
// 按定长报告裁剪，vendor bulk 无长度上限（分多次写入 FIFO）。
void usb_resp_sink(const u8 *data, u16 len, void *arg)
{
    auto &ctx = *static_cast<DapTaskContext *>(arg);

    dap_send_response(ctx.source, data, len);
    Application::get_instance().config_channel().add_dap_command(ctx.request_len, len);
}

void dap_task(void *arg)
{
    // 复用公共执行器：命令源 = USB 命令队列（两条传输通道共用；Wireless 模式
    // 在源内提交给配对 peer），响应宿 = 按来源写回对应通道；请求/响应缓冲用本
    // 文件传输层缓冲（不占任务栈）。执行循环、DAP_Setup 与命令日志均在
    // dap_executor.cc。
    DapTaskContext ctx = {};
    ctx.usb = reinterpret_cast<mlink::CmsisDapUsb *>(arg);
    mlink::dap_executor_run(usb_cmd_src, usb_resp_sink, &ctx, dap_request, sizeof(dap_request), dap_response,
                            sizeof(dap_response));
}

} // namespace

void tud_vendor_tx_cb(u8 idx, u32 sent_bytes)
{
    (void)sent_bytes;
    if (idx == USB_DAP_INTERFACE)
    {
        s_vendor_tx_bytes = s_vendor_tx_bytes + sent_bytes;
    }
}

void tud_vendor_rx_cb(u8 idx, const u8 *buffer, u16 buffer_size)
{
    (void)buffer;
    (void)buffer_size;

    // 更新模式：唯一的 vendor 接口由 UpdateMode 接管（本轮只把 FIFO 读空），
    // 分流必须发生在 Application 之前——更新模式没有构造 Application，取它
    // 会连带初始化 NVS、无线与串口桥
    if (auto *update = mlink::UpdateMode::try_instance())
    {
        update->usb_rx(idx);
        return;
    }

    if (idx != USB_DAP_INTERFACE)
    {
        return;
    }

    // FIFO 模式（CFG_TUD_VENDOR_TXRX_BUFFERED=1）：回调不携带数据，
    // 必须从内部 RX FIFO 读取。tud_vendor_n_read 仅在该模式下可用。
    // 读到的字节流按 DAP 命令自描述长度重组：一条命令可能跨多次回调到达，
    // 一次回调也可能带回多条命令；重组后由 dap_task 与 HID 通道的命令
    // 一起顺序执行。
    u8 tmp[CONFIG_MEGUMINLINK_USB_MAX_PACKET_SIZE];
    u32 n;
    while ((n = tud_vendor_n_read(idx, tmp, sizeof(tmp))) > 0)
    {
        accumulate_dap_command(tmp, static_cast<size_t>(n));
    }
}

// HID 类回调：TinyUSB 只对 report_complete/protocol/idle 提供弱默认实现，
// 这三个是硬链接依赖，TINYUSB_HID_COUNT>0 时必须由应用提供（HID 接口关闭时
// 同样要定义——HID 类代码已编入固件，只是没有接口引用它）。
#if CONFIG_TINYUSB_HID_COUNT > 0
u8 const *tud_hid_descriptor_report_cb([[maybe_unused]] u8 instance)
{
    // HID 接口未暴露（MEGUMINLINK_USB_HID_DAP_ENABLE=n）时为 nullptr：描述符里
    // 没有 HID 接口，本回调不会被调用。
    return Application::get_instance().usb().descriptors().hid_report();
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
    // SET_REPORT 两条路径都进这里，统一入队交给 dap_task。未暴露时（兼容模式
    // 关闭）收不到报告，残留的控制请求直接丢弃。
    if (!Application::get_instance().usb().hid_exposed() || instance != HID_INSTANCE ||
        report_type != HID_REPORT_TYPE_OUTPUT || bufsize == 0)
    {
        return;
    }
    enqueue_dap_command(DapSource::Hid, buffer, bufsize);
#endif
}
#endif // CONFIG_TINYUSB_HID_COUNT > 0

const u8 *tud_descriptor_bos_cb()
{
    // 更新模式不宣告平台能力（其描述符只报 USB 2.0，主机不会请求 BOS）
    if (auto *update = mlink::UpdateMode::try_instance())
    {
        return update->usb_bos();
    }

    // BOS 按运行时的能力状态构造（见 usb_capability.cc）：主机只在枚举时读取，
    // 运行时开关变化靠 apply_usb_capability 的重新枚举让它重读
    return mlink::Application::get_instance().usb().descriptors().capability().bos();
}

bool tud_vendor_control_xfer_cb(u8 rhport, u8 stage, const tusb_control_request_t *request)
{
    // 更新模式的接入点在 Application 之前：该模式没有配置通道，也不存在
    // Application 实例（见 UpdateMode::usb_control）
    if (auto *update = mlink::UpdateMode::try_instance())
    {
        return update->usb_control(rhport, stage, request);
    }

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
    const UsbCapabilityDescriptors &capability_descriptors = usb.descriptors().capability();

    switch (request->bRequest)
    {
#if CONFIG_MEGUMINLINK_USB_WINUSB_CAPABILITY_ENABLE
    case VENDOR_CODE_MS_OS_20:
        if (request->wIndex == MS_OS_20_OP_GET_DESCRIPTOR_SET && usb.usb_capability().winusb != 0)
        {
            tud_control_xfer(rhport, request, const_cast<u8 *>(capability_descriptors.ms_os_descriptor_set()),
                             capability_descriptors.ms_os_descriptor_set_len());
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
            tud_control_xfer(rhport, request, const_cast<u8 *>(capability_descriptors.url_descriptor()),
                             capability_descriptors.url_descriptor_len());
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
    // 能力状态从 NVS 载入（无记录用编译期默认值），并据此构造描述符集：必须在
    // tinyusb_driver_install（init）之前完成，主机枚举时读到的就是它
    m_usb_capability = usb_capability_load();
    m_descriptors.rebuild(m_usb_capability);

    // 命令队列按字节容量创建：每条命令最多占一个 DapCommand（长度字段 +
    // MAX_DAP_PAYLOAD_SIZE 数据段）。容量必须容得下至少一条完整命令，
    // 否则 NOSPLIT 环形缓冲会直接拒绝入队。
    constexpr size_t RINGBUF_SIZE = sizeof(DapCommand) * CONFIG_MEGUMINLINK_PACKET_BUFFER_DEPTH;

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

    ret = m_descriptors.load_serial();
    ESP_RETURN_ON_ERROR(ret, TAG, "Failed to read MAC address");

    {
        tinyusb_config_t tinyusb_config = TINYUSB_DEFAULT_CONFIG(tinyusb_event_callback);
        tinyusb_config.descriptor.device = &m_descriptors.device();
        tinyusb_config.descriptor.string = m_descriptors.string_table();
        tinyusb_config.descriptor.string_count = m_descriptors.string_count();
        tinyusb_config.descriptor.full_speed_config = m_descriptors.config();

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
    ret = m_cdc.init();
    ESP_RETURN_ON_ERROR(ret, TAG, "Failed to initialize CDC ACM interface");
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
    // 这里只挂起 DAP 任务：本机串口桥（UartBridge）有它自己的任务，且 Local 模式下
    // 串口直通与 USB 是否在总线上无关，不随本开关启停。
    if (enabled)
    {
        tud_connect();
        if (m_dap_task != nullptr)
        {
            vTaskResume(m_dap_task);
        }
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
        ESP_LOGI(TAG, "USB disabled (wireless debug mode)");
    }
}

void CmsisDapUsb::apply_usb_capability(const UsbCapabilitySettings &settings)
{
    // 断开窗口内重建：主机收到软断开后停止请求描述符，描述符缓冲与字符串内容不会
    // 被 TinyUSB 任务并发读取；重建完成再连接，主机重新枚举并重读
    tud_disconnect();
    vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MS));

    m_usb_capability = settings;
    m_descriptors.rebuild(m_usb_capability);

    tud_connect();
    ESP_LOGI(TAG, "USB capabilities applied (webusb=%u winusb=%u hid_compat=%u landing_page=%s), re-enumerating",
             static_cast<unsigned>(m_usb_capability.webusb), static_cast<unsigned>(m_usb_capability.winusb),
             static_cast<unsigned>(m_usb_capability.hid_compat), m_usb_capability.landing_page);
}
} // namespace mlink

/**
 * DAP_Info 的产品名（DAP_config.h 的 DAP_GetProductString 调用它）。
 *
 * 声明在 DAP_config.h 里以求 C 链接：DAP.c 是 C 编译单元，拿不到 C++ 对象。
 */
extern "C" const char *mlink_dap_product_name(void)
{
    return mlink::Application::get_instance().usb().dap_product_name();
}
