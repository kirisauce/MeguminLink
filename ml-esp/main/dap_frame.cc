#include "meguminlink/dap_frame.hpp"

// 注意 include 顺序：DAP_config.h 必须先于 DAP.h（与 usb.cc / wireless/link.cc
// 一致）。DAP_config.h 提供 DELAY_SLOW_CYCLES/PIN_DELAY_SLOW 等平台实现，
// DAP.h 仅在未定义时提供默认值（#ifndef 保护）；顺序反了会触发宏重定义。
// clang-format off
#include "DAP_config.h"
#include "DAP.h"
// clang-format on

namespace
{
/** 数据不足：调用方继续累积 */
constexpr size_t NEED_MORE = 0;

/** 解析结果：完整命令长度 + 该命令的响应长度上界 */
struct FrameInfo
{
    size_t size;
    size_t bound;
};

constexpr FrameInfo need_more()
{
    return FrameInfo{NEED_MORE, 0};
}

constexpr FrameInfo frame(size_t size, size_t bound)
{
    return FrameInfo{size, bound};
}

/** avail 是否已够 n 字节 */
constexpr bool enough(size_t avail, size_t n)
{
    return avail >= n;
}

/** 按命令 ID 解析；allow_batch 决定是否把子命令序列递归展开 */
FrameInfo parse_frame(const u8 *request, size_t avail, bool allow_batch);

/**
 * DAP_Transfer（0x05）：[ID][DAP Index][Transfer Count] 后跟 Count 组传输。
 * SWD 与 JTAG 的请求布局一致（见 DAP.c 的 DAP_SWD_Transfer / DAP_JTAG_Transfer）。
 * 响应为 [ID][Count][Response] 加各读传输的数据。
 */
FrameInfo transfer_frame(const u8 *request, size_t avail)
{
    if (!enough(avail, 3))
    {
        return need_more();
    }

    const size_t count = request[2];
    size_t size = 3;
    size_t bound = 3;
    for (size_t i = 0; i < count; i++)
    {
        if (!enough(avail, size + 1))
        {
            return need_more();
        }
        const u8 value = request[size];
        size++;

        if ((value & DAP_TRANSFER_RnW) != 0U)
        {
            // 读传输产出 4B 数据；置 TIMESTAMP 时再 4B。时间戳只在特定
            // 读序列上出现，此处按最坏情况一律计入。
            bound += 8;
            if ((value & DAP_TRANSFER_MATCH_VALUE) != 0U)
            {
                if (!enough(avail, size + 4))
                {
                    return need_more();
                }
                size += 4;
            }
        }
        else
        {
            if (!enough(avail, size + 4))
            {
                return need_more();
            }
            size += 4;
        }
    }
    return frame(size, bound);
}

/**
 * DAP_TransferBlock（0x06）：[ID][DAP Index][Count L][Count H][Request]
 * 加写时的 Count×4B 数据。响应为 [ID][Count L][Count H][Response] 加读
 * 出的数据，因此读时上界随 16 位 Count 线性增长。
 */
FrameInfo transfer_block_frame(const u8 *request, size_t avail)
{
    if (!enough(avail, 5))
    {
        return need_more();
    }

    const size_t count = static_cast<size_t>(request[2]) | (static_cast<size_t>(request[3]) << 8);
    size_t size = 5;
    size_t bound = 4;
    if ((request[4] & DAP_TRANSFER_RnW) != 0U)
    {
        bound += count * 4U;
    }
    else
    {
        size += count * 4U;
    }
    return enough(avail, size) ? frame(size, bound) : need_more();
}

/** DAP_SWJ_Sequence（0x12）：[ID][Sequence Count] + ceil(count/8)B，count 为 0 表示 256 位。响应恒为 [ID][OK] */
FrameInfo swj_sequence_frame(const u8 *request, size_t avail)
{
    if (!enough(avail, 2))
    {
        return need_more();
    }

    const size_t bits = (request[1] == 0U) ? 256U : static_cast<size_t>(request[1]);
    const size_t size = 2 + ((bits + 7U) / 8U);
    return enough(avail, size) ? frame(size, 2) : need_more();
}

/**
 * DAP_JTAG_Sequence（0x14）：[ID][Sequence Count] + 每组 [Info] + ceil(TCK/8)B。
 * 响应为 [ID][OK] 加各 TDO 捕获序列的数据。
 */
FrameInfo jtag_sequence_frame(const u8 *request, size_t avail)
{
    if (!enough(avail, 2))
    {
        return need_more();
    }

    const size_t sequence_count = request[1];
    size_t size = 2;
    size_t bound = 2;
    for (size_t i = 0; i < sequence_count; i++)
    {
        if (!enough(avail, size + 1))
        {
            return need_more();
        }
        const u8 info = request[size];
        size++;

        size_t tck = info & JTAG_SEQUENCE_TCK;
        if (tck == 0U)
        {
            tck = 64U;
        }
        const size_t bytes = (tck + 7U) / 8U;
        size += bytes;
        if ((info & JTAG_SEQUENCE_TDO) != 0U)
        {
            bound += bytes;
        }
        if (!enough(avail, size))
        {
            return need_more();
        }
    }
    return frame(size, bound);
}

/** DAP_JTAG_Configure（0x15）：[ID][Device Count] + Count 个 IR 长度字节。响应恒为 [ID][OK] */
FrameInfo jtag_configure_frame(const u8 *request, size_t avail)
{
    if (!enough(avail, 2))
    {
        return need_more();
    }

    const size_t size = 2 + static_cast<size_t>(request[1]);
    return enough(avail, size) ? frame(size, 2) : need_more();
}

/**
 * DAP_SWD_Sequence（0x1D）：[ID][Sequence Count] + 每组 [Info] 加输出序列的
 * 数据字节。响应为 [ID][OK] 加各捕获序列（SWDIO 输入）的数据。
 */
FrameInfo swd_sequence_frame(const u8 *request, size_t avail)
{
    if (!enough(avail, 2))
    {
        return need_more();
    }

    const size_t sequence_count = request[1];
    size_t size = 2;
    size_t bound = 2;
    for (size_t i = 0; i < sequence_count; i++)
    {
        if (!enough(avail, size + 1))
        {
            return need_more();
        }
        const u8 info = request[size];
        size++;

        size_t clk = info & SWD_SEQUENCE_CLK;
        if (clk == 0U)
        {
            clk = 64U;
        }
        const size_t bytes = (clk + 7U) / 8U;

        // 捕获序列（SWDIO 输入）不携带请求数据，但在响应里回读数据
        if ((info & SWD_SEQUENCE_DIN) != 0U)
        {
            bound += bytes;
            continue;
        }
        size += bytes;
        if (!enough(avail, size))
        {
            return need_more();
        }
    }
    return frame(size, bound);
}

/**
 * DAP_ExecuteCommands（0x7F）：[ID][Command Count] + 子命令序列，响应为
 * [ID][Count] 加各子命令响应的拼接。子命令按嵌套规则解析：参考实现的
 * DAP_ProcessCommand 不处理 0x7F，嵌套的 0x7F 会被当作未定义命令消耗
 * 1 字节，故此处同样不展开（同时避免递归无界）。
 */
FrameInfo batch_frame(const u8 *request, size_t avail)
{
    if (!enough(avail, 2))
    {
        return need_more();
    }

    const size_t command_count = request[1];
    size_t size = 2;
    size_t bound = 2;
    for (size_t i = 0; i < command_count; i++)
    {
        const FrameInfo sub = parse_frame(request + size, avail - size, false);
        if (sub.size == NEED_MORE)
        {
            return need_more();
        }
        size += sub.size;
        bound += sub.bound;
    }
    return frame(size, bound);
}

FrameInfo parse_frame(const u8 *request, size_t avail, bool allow_batch)
{
    if (request == nullptr || avail == 0U)
    {
        return need_more();
    }

    switch (request[0])
    {
    case ID_DAP_Info:
        // 响应 [ID][Length] + DAP_Info 数据（最长 12B vendor 字符串）
        return enough(avail, 2) ? frame(2, 14) : need_more();
    case ID_DAP_HostStatus:
        return enough(avail, 3) ? frame(3, 2) : need_more();
    case ID_DAP_Connect:
        return enough(avail, 2) ? frame(2, 2) : need_more();
    case ID_DAP_Disconnect:
        return frame(1, 2);
    case ID_DAP_TransferConfigure:
        return enough(avail, 6) ? frame(6, 2) : need_more();
    case ID_DAP_Transfer:
        return transfer_frame(request, avail);
    case ID_DAP_TransferBlock:
        return transfer_block_frame(request, avail);
    case ID_DAP_TransferAbort:
        return frame(1, 2);
    case ID_DAP_WriteABORT:
        return enough(avail, 6) ? frame(6, 2) : need_more();
    case ID_DAP_Delay:
        return enough(avail, 3) ? frame(3, 2) : need_more();
    case ID_DAP_ResetTarget:
        return frame(1, 3);
    case ID_DAP_SWJ_Pins:
        return enough(avail, 7) ? frame(7, 2) : need_more();
    case ID_DAP_SWJ_Clock:
        return enough(avail, 5) ? frame(5, 2) : need_more();
    case ID_DAP_SWJ_Sequence:
        return swj_sequence_frame(request, avail);
    case ID_DAP_SWD_Configure:
        return enough(avail, 2) ? frame(2, 2) : need_more();
    case ID_DAP_SWD_Sequence:
        return swd_sequence_frame(request, avail);
    case ID_DAP_JTAG_Sequence:
        return jtag_sequence_frame(request, avail);
    case ID_DAP_JTAG_Configure:
        return jtag_configure_frame(request, avail);
    case ID_DAP_JTAG_IDCODE:
        // 响应 [ID][OK] + 4B IDCODE
        return enough(avail, 2) ? frame(2, 6) : need_more();
    case ID_DAP_SWO_Transport:
        // [ID][Transport] → [ID][OK/ERR]
        return enough(avail, 2) ? frame(2, 2) : need_more();
    case ID_DAP_SWO_Mode:
        // [ID][Mode] → [ID][OK/ERR]
        return enough(avail, 2) ? frame(2, 2) : need_more();
    case ID_DAP_SWO_Baudrate:
        // [ID][Baudrate 4B] → [ID][Baudrate 4B]
        return enough(avail, 5) ? frame(5, 5) : need_more();
    case ID_DAP_SWO_Control:
        // [ID][Control] → [ID][OK/ERR]
        return enough(avail, 2) ? frame(2, 2) : need_more();
    case ID_DAP_SWO_Status:
        // 无请求参数 → [ID][Status][Count 4B]
        return frame(1, 6);
    case ID_DAP_SWO_ExtendedStatus:
        // [ID][Cmd] → [ID] + 按 Cmd 位回 Status(1B)/Count(4B)/Timestamp(8B)
        return enough(avail, 2) ? frame(2, 14) : need_more();
    case ID_DAP_SWO_Data: {
        // [ID][Count 2B] → [ID][Status][Count 2B] + 数据；数据段在 SWO_Data 内
        // 被截到 DAP_PACKET_SIZE-4（上游 SWO.c 按 v1 包尺寸的固定上限）
        return enough(avail, 3) ? frame(3, 3U + DAP_PACKET_SIZE - 4U) : need_more();
    }
    case ID_DAP_ExecuteCommands:
        return allow_batch ? batch_frame(request, avail) : frame(1, 1);
    default:
        // DAP UART / 供应商命令与未定义命令：本固件的 DAP_config.h 中
        // SWO_MANCHESTER、DAP_UART 均为 0，DAP_ProcessCommand 不含
        // 这些分支，按单字节命令消耗并回 Invalid，此处保持同一行为。
        return frame(1, 1);
    }
}
} // namespace

size_t mlink::dap_request_size(const u8 *request, size_t avail)
{
    return parse_frame(request, avail, true).size;
}

size_t mlink::dap_response_bound(const u8 *request, size_t avail)
{
    return parse_frame(request, avail, true).bound;
}

const char *mlink::dap_command_name(u8 id)
{
    switch (id)
    {
    case ID_DAP_Info:
        return "DAP_Info";
    case ID_DAP_HostStatus:
        return "DAP_HostStatus";
    case ID_DAP_Connect:
        return "DAP_Connect";
    case ID_DAP_Disconnect:
        return "DAP_Disconnect";
    case ID_DAP_TransferConfigure:
        return "DAP_TransferConfigure";
    case ID_DAP_Transfer:
        return "DAP_Transfer";
    case ID_DAP_TransferBlock:
        return "DAP_TransferBlock";
    case ID_DAP_TransferAbort:
        return "DAP_TransferAbort";
    case ID_DAP_WriteABORT:
        return "DAP_WriteABORT";
    case ID_DAP_Delay:
        return "DAP_Delay";
    case ID_DAP_ResetTarget:
        return "DAP_ResetTarget";
    case ID_DAP_SWJ_Pins:
        return "DAP_SWJ_Pins";
    case ID_DAP_SWJ_Clock:
        return "DAP_SWJ_Clock";
    case ID_DAP_SWJ_Sequence:
        return "DAP_SWJ_Sequence";
    case ID_DAP_SWD_Configure:
        return "DAP_SWD_Configure";
    case ID_DAP_JTAG_Sequence:
        return "DAP_JTAG_Sequence";
    case ID_DAP_JTAG_Configure:
        return "DAP_JTAG_Configure";
    case ID_DAP_JTAG_IDCODE:
        return "DAP_JTAG_IDCODE";
    case ID_DAP_SWO_Transport:
        return "DAP_SWO_Transport";
    case ID_DAP_SWO_Mode:
        return "DAP_SWO_Mode";
    case ID_DAP_SWO_Baudrate:
        return "DAP_SWO_Baudrate";
    case ID_DAP_SWO_Control:
        return "DAP_SWO_Control";
    case ID_DAP_SWO_Status:
        return "DAP_SWO_Status";
    case ID_DAP_SWO_Data:
        return "DAP_SWO_Data";
    case ID_DAP_SWD_Sequence:
        return "DAP_SWD_Sequence";
    case ID_DAP_SWO_ExtendedStatus:
        return "DAP_SWO_ExtendedStatus";
    case ID_DAP_UART_Transport:
        return "DAP_UART_Transport";
    case ID_DAP_UART_Configure:
        return "DAP_UART_Configure";
    case ID_DAP_UART_Transfer:
        return "DAP_UART_Transfer";
    case ID_DAP_UART_Control:
        return "DAP_UART_Control";
    case ID_DAP_UART_Status:
        return "DAP_UART_Status";
    case ID_DAP_QueueCommands:
        return "DAP_QueueCommands";
    case ID_DAP_ExecuteCommands:
        return "DAP_ExecuteCommands";
    default:
        return "unknown";
    }
}
