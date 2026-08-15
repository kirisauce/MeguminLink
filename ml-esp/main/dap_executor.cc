#include "meguminlink/dap_executor.hpp"
#include "esp_log.h"
#include "meguminlink/wireless_link.hpp" // MAX_DAP_PAYLOAD_SIZE（响应缓冲）

// 注意 include 顺序：DAP_config.h 必须先于 DAP.h（与 usb.cc 一致）。
// DAP_config.h 提供 DELAY_SLOW_CYCLES/PIN_DELAY_SLOW 等平台实现，
// DAP.h 仅在未定义时提供默认值（#ifndef 保护）；顺序反了会触发宏重定义。
// clang-format 的字典序排序会把它们排反，此处保持顺序：
// clang-format off
#include "DAP_config.h"
#include "DAP.h"
// clang-format on

using namespace mlink;

static constexpr char TAG[] = "mlink::dap_executor";

namespace
{
/**
 * DAP 命令号 → 可读名称（CMSIS-DAP v2 命令 ID，见 DAP.h 的 ID_DAP_*）。
 * 执行日志用；与 cmsis_dap 实现解耦，不依赖其 ID 宏。
 */
const char *dap_command_name(u8 id)
{
    switch (id)
    {
    case 0x00:
        return "DAP_Info";
    case 0x01:
        return "DAP_HostStatus";
    case 0x02:
        return "DAP_Connect";
    case 0x03:
        return "DAP_Disconnect";
    case 0x04:
        return "DAP_TransferConfigure";
    case 0x05:
        return "DAP_Transfer";
    case 0x06:
        return "DAP_TransferBlock";
    case 0x07:
        return "DAP_TransferAbort";
    case 0x08:
        return "DAP_WriteABORT";
    case 0x09:
        return "DAP_Delay";
    case 0x0A:
        return "DAP_ResetTarget";
    case 0x10:
        return "DAP_SWJ_Pins";
    case 0x11:
        return "DAP_SWJ_Clock";
    case 0x12:
        return "DAP_SWJ_Sequence";
    case 0x13:
        return "DAP_SWD_Configure";
    case 0x14:
        return "DAP_JTAG_Sequence";
    case 0x15:
        return "DAP_JTAG_Configure";
    case 0x16:
        return "DAP_JTAG_IDCODE";
    case 0x17:
        return "DAP_SWO_Transport";
    case 0x18:
        return "DAP_SWO_Mode";
    case 0x19:
        return "DAP_SWO_Baudrate";
    case 0x1A:
        return "DAP_SWO_Control";
    case 0x1B:
        return "DAP_SWO_Status";
    case 0x1C:
        return "DAP_SWO_Data";
    case 0x1D:
        return "DAP_SWD_Sequence";
    case 0x1E:
        return "DAP_SWO_ExtendedStatus";
    case 0x1F:
        return "DAP_UART_Transport";
    case 0x20:
        return "DAP_UART_Configure";
    case 0x21:
        return "DAP_UART_Transfer";
    case 0x22:
        return "DAP_UART_Control";
    case 0x23:
        return "DAP_UART_Status";
    case 0x7E:
        return "DAP_QueueCommands";
    case 0x7F:
        return "DAP_ExecuteCommands";
    default:
        return "unknown";
    }
}
} // namespace

u16 mlink::dap_execute_once(const u8 *request, u16 req_len, u8 *response, size_t response_cap)
{
    // 每命令两行日志降为 DEBUG（默认不打印）：A/B 实测（2026-08-15）日志
    // 降级单独贡献 ~0.2s（UART FIFO 异步排空，非阻塞路径）。
    ESP_LOGD(TAG, "Executing DAP cmd: id=0x%02X (%s) len=%u", request[0], dap_command_name(request[0]),
             static_cast<unsigned>(req_len));
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, request, req_len, ESP_LOG_DEBUG);

    const u32 n = DAP_ExecuteCommand(request, response);
    const u16 resp_len = static_cast<u16>(n & 0xFFFFU);
    if (resp_len > response_cap)
    {
        // 调用方缓冲不足：写入已发生（CMSIS-DAP 批量响应不检查边界），
        // 按容量截断返回并告警。调用方应提供 DAP_MAX_RESPONSE_SIZE 容量
        // 的缓冲（请求 ≤ DAP_PACKET_SIZE 时不会触发）。
        ESP_LOGW(TAG, "DAP response truncated (%u -> %u)", resp_len, static_cast<unsigned>(response_cap));
        return static_cast<u16>(response_cap);
    }
    ESP_LOGD(TAG, "DAP cmd 0x%02X done, response %u bytes", request[0], static_cast<unsigned>(resp_len));
    return resp_len;
}

void mlink::dap_executor_run(DapCommandSource cmd_src, DapResponseSink resp_sink, void *arg, u8 *request,
                             size_t request_cap, u8 *response, size_t response_cap)
{
    DAP_Setup(); // 初始化 DAP 状态与 SWJ 时钟（与 usb.cc 的 dap_task 一致）

    while (true)
    {
        u16 req_len = 0;
        if (!cmd_src(request, request_cap, &req_len, arg))
        {
            continue;
        }

        const u16 resp_len = dap_execute_once(request, req_len, response, response_cap);
        if (resp_len > MAX_DAP_PAYLOAD_SIZE)
        {
            // 无线帧承载上限（ESP-NOW v2 1467B 载荷）装不下：丢弃，
            // PC 端 debugger 会超时并自行重试（发送截断响应只会破坏协议）
            ESP_LOGW(TAG, "DAP response too large (%u bytes, max %u), dropped", static_cast<unsigned>(resp_len),
                     static_cast<unsigned>(MAX_DAP_PAYLOAD_SIZE));
            continue;
        }
        resp_sink(response, resp_len, arg);
    }
}
