#pragma once
#ifndef __MEGUMINLINK_DAP_FRAME_HPP__
#define __MEGUMINLINK_DAP_FRAME_HPP__

#include "meguminlink/common.hpp"

#include <cstddef> // size_t

namespace mlink
{
/**
 * 计算一条 CMSIS-DAP 请求的完整字节数（含命令 ID）。
 *
 * USB bulk 端点小于命令本身时，一条命令会跨多个 USB 包到达；接收侧必须
 * 按命令自描述长度把字节流重新切分成完整命令，否则半条命令会被当成独立
 * 请求执行。本函数只解析长度：不访问目标、不读写 DAP 状态，可安全用于
 * 收包上下文。
 *
 * 长度规则与 cmsis_dap/Source/DAP.c 的解析保持同一套命令 ID 与字段布局：
 * 固定长度命令直接给出，变长命令（Transfer / TransferBlock / 序列类 /
 * 0x7F 批量）按请求内的计数字段推进。
 *
 * @param request 已收到的字节流起始位置
 * @param avail   已收到的字节数
 * @return 完整命令的字节数；数据不足时返回 0（调用方继续累积）
 */
size_t dap_request_size(const u8 *request, size_t avail);
} // namespace mlink

#endif
