#include "meguminlink/dap_executor.hpp"
#include "meguminlink/dap_frame.hpp"
#include "esp_log.h"
#include "meguminlink/wireless/link.hpp" // MAX_DAP_PAYLOAD_SIZE（响应缓冲）

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

u16 mlink::dap_execute_once(const u8 *request, u16 req_len, u8 *response, size_t response_cap)
{
    // DAP 层写响应不检查缓冲区边界，先确认该请求的响应上界装得下：超限时
    // 按未定义命令的语义回单字节 ID_DAP_Invalid（与 DAP_ProcessCommand 的
    // 默认分支一致），避免写坏相邻内存。
    const size_t bound = dap_response_bound(request, req_len);
    if (bound == 0U || bound > response_cap)
    {
        ESP_LOGW(TAG, "DAP cmd 0x%02X rejected: response bound %u exceeds buffer %u", request[0],
                 static_cast<unsigned>(bound), static_cast<unsigned>(response_cap));
        response[0] = ID_DAP_Invalid;
        return 1;
    }

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
            // 无线单帧载荷上限装不下：丢弃，PC 端 debugger 会超时并自行重试
            // （发送截断响应只会破坏协议）
            ESP_LOGW(TAG, "DAP response too large (%u bytes, max %u), dropped", static_cast<unsigned>(resp_len),
                     static_cast<unsigned>(MAX_DAP_PAYLOAD_SIZE));
            continue;
        }
        resp_sink(response, resp_len, arg);
    }
}
