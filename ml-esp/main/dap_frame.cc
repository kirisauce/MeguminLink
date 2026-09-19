#include "meguminlink/dap_frame.hpp"

// 注意 include 顺序：DAP_config.h 必须先于 DAP.h（与 usb.cc / wireless_link.cc
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

/** avail 是否已够 n 字节 */
constexpr bool enough(size_t avail, size_t n)
{
    return avail >= n;
}

/** 按命令 ID 解析长度；allow_batch 决定是否把子命令序列递归展开 */
size_t parse_size(const u8 *request, size_t avail, bool allow_batch);

/**
 * DAP_Transfer（0x05）：[ID][DAP Index][Transfer Count] 后跟 Count 组传输。
 * SWD 与 JTAG 的请求布局一致（见 DAP.c 的 DAP_SWD_Transfer / DAP_JTAG_Transfer）。
 */
size_t transfer_size(const u8 *request, size_t avail)
{
    if (!enough(avail, 3))
    {
        return NEED_MORE;
    }

    const size_t count = request[2];
    size_t size = 3;
    for (size_t i = 0; i < count; i++)
    {
        if (!enough(avail, size + 1))
        {
            return NEED_MORE;
        }
        const u8 value = request[size];
        size++;

        // 写传输总是跟 4B 数据；读传输仅在置 MATCH_VALUE 时跟 4B 匹配值
        size_t payload = 4;
        if ((value & DAP_TRANSFER_RnW) != 0U)
        {
            payload = ((value & DAP_TRANSFER_MATCH_VALUE) != 0U) ? 4U : 0U;
        }
        if (!enough(avail, size + payload))
        {
            return NEED_MORE;
        }
        size += payload;
    }
    return size;
}

/** DAP_TransferBlock（0x06）：[ID][DAP Index][Count L][Count H][Request] + 写时 Count×4B */
size_t transfer_block_size(const u8 *request, size_t avail)
{
    if (!enough(avail, 5))
    {
        return NEED_MORE;
    }

    size_t size = 5;
    if ((request[4] & DAP_TRANSFER_RnW) == 0U)
    {
        const size_t count = static_cast<size_t>(request[2]) | (static_cast<size_t>(request[3]) << 8);
        size += count * 4U;
    }
    return enough(avail, size) ? size : NEED_MORE;
}

/** DAP_SWJ_Sequence（0x12）：[ID][Sequence Count] + ceil(count/8)B，count 为 0 表示 256 位 */
size_t swj_sequence_size(const u8 *request, size_t avail)
{
    if (!enough(avail, 2))
    {
        return NEED_MORE;
    }

    const size_t bits = (request[1] == 0U) ? 256U : static_cast<size_t>(request[1]);
    const size_t size = 2 + ((bits + 7U) / 8U);
    return enough(avail, size) ? size : NEED_MORE;
}

/** DAP_JTAG_Sequence（0x14）：[ID][Sequence Count] + 每组 [Info] + ceil(TCK/8)B */
size_t jtag_sequence_size(const u8 *request, size_t avail)
{
    if (!enough(avail, 2))
    {
        return NEED_MORE;
    }

    const size_t sequence_count = request[1];
    size_t size = 2;
    for (size_t i = 0; i < sequence_count; i++)
    {
        if (!enough(avail, size + 1))
        {
            return NEED_MORE;
        }
        const u8 info = request[size];
        size++;

        size_t tck = info & JTAG_SEQUENCE_TCK;
        if (tck == 0U)
        {
            tck = 64U;
        }
        size += (tck + 7U) / 8U;
        if (!enough(avail, size))
        {
            return NEED_MORE;
        }
    }
    return size;
}

/** DAP_JTAG_Configure（0x15）：[ID][Device Count] + Count 个 IR 长度字节 */
size_t jtag_configure_size(const u8 *request, size_t avail)
{
    if (!enough(avail, 2))
    {
        return NEED_MORE;
    }

    const size_t size = 2 + static_cast<size_t>(request[1]);
    return enough(avail, size) ? size : NEED_MORE;
}

/** DAP_SWD_Sequence（0x1D）：[ID][Sequence Count] + 每组 [Info] + 输出序列的数据字节 */
size_t swd_sequence_size(const u8 *request, size_t avail)
{
    if (!enough(avail, 2))
    {
        return NEED_MORE;
    }

    const size_t sequence_count = request[1];
    size_t size = 2;
    for (size_t i = 0; i < sequence_count; i++)
    {
        if (!enough(avail, size + 1))
        {
            return NEED_MORE;
        }
        const u8 info = request[size];
        size++;

        // 捕获序列（SWDIO 输入）不携带请求数据
        if ((info & SWD_SEQUENCE_DIN) != 0U)
        {
            continue;
        }
        size_t clk = info & SWD_SEQUENCE_CLK;
        if (clk == 0U)
        {
            clk = 64U;
        }
        size += (clk + 7U) / 8U;
        if (!enough(avail, size))
        {
            return NEED_MORE;
        }
    }
    return size;
}

/**
 * DAP_ExecuteCommands（0x7F）：[ID][Command Count] + 子命令序列。
 * 子命令按嵌套规则解析：参考实现的 DAP_ProcessCommand 不处理 0x7F，嵌套的
 * 0x7F 会被当作未定义命令消耗 1 字节，故此处同样不展开（同时避免递归无界）。
 */
size_t batch_size(const u8 *request, size_t avail)
{
    if (!enough(avail, 2))
    {
        return NEED_MORE;
    }

    const size_t command_count = request[1];
    size_t size = 2;
    for (size_t i = 0; i < command_count; i++)
    {
        const size_t sub = parse_size(request + size, avail - size, false);
        if (sub == NEED_MORE)
        {
            return NEED_MORE;
        }
        size += sub;
    }
    return size;
}

size_t parse_size(const u8 *request, size_t avail, bool allow_batch)
{
    if (request == nullptr || avail == 0U)
    {
        return NEED_MORE;
    }

    switch (request[0])
    {
    case ID_DAP_Info:
        return enough(avail, 2) ? 2 : NEED_MORE;
    case ID_DAP_HostStatus:
        return enough(avail, 3) ? 3 : NEED_MORE;
    case ID_DAP_Connect:
        return enough(avail, 2) ? 2 : NEED_MORE;
    case ID_DAP_Disconnect:
        return 1;
    case ID_DAP_TransferConfigure:
        return enough(avail, 6) ? 6 : NEED_MORE;
    case ID_DAP_Transfer:
        return transfer_size(request, avail);
    case ID_DAP_TransferBlock:
        return transfer_block_size(request, avail);
    case ID_DAP_TransferAbort:
        return 1;
    case ID_DAP_WriteABORT:
        return enough(avail, 6) ? 6 : NEED_MORE;
    case ID_DAP_Delay:
        return enough(avail, 3) ? 3 : NEED_MORE;
    case ID_DAP_ResetTarget:
        return 1;
    case ID_DAP_SWJ_Pins:
        return enough(avail, 7) ? 7 : NEED_MORE;
    case ID_DAP_SWJ_Clock:
        return enough(avail, 5) ? 5 : NEED_MORE;
    case ID_DAP_SWJ_Sequence:
        return swj_sequence_size(request, avail);
    case ID_DAP_SWD_Configure:
        return enough(avail, 2) ? 2 : NEED_MORE;
    case ID_DAP_SWD_Sequence:
        return swd_sequence_size(request, avail);
    case ID_DAP_JTAG_Sequence:
        return jtag_sequence_size(request, avail);
    case ID_DAP_JTAG_Configure:
        return jtag_configure_size(request, avail);
    case ID_DAP_JTAG_IDCODE:
        return enough(avail, 2) ? 2 : NEED_MORE;
    case ID_DAP_ExecuteCommands:
        return allow_batch ? batch_size(request, avail) : 1;
    default:
        // SWO / DAP UART / 供应商命令与未定义命令：本固件的 DAP_config.h 中
        // SWO_UART、SWO_MANCHESTER、DAP_UART 均为 0，DAP_ProcessCommand 不含
        // 这些分支，按单字节命令消耗并回 Invalid，此处保持同一行为。
        return 1;
    }
}
} // namespace

size_t mlink::dap_request_size(const u8 *request, size_t avail)
{
    return parse_size(request, avail, true);
}
