#pragma once
#ifndef __MEGUMINLINK_DAP_FRAME_HPP__
#define __MEGUMINLINK_DAP_FRAME_HPP__

#include "meguminlink/common.hpp"

#include <cstddef> // size_t

namespace mlink
{
/**
 * 上报给主机的单包尺寸上限（DAP_Info 的 Packet Size）。
 *
 * CMSIS-DAP 只用单个字节表示 DAP_Transfer 的 Transfer Count（1..255），主机据此
 * 把一次批量传输拆成多条命令。OpenOCD 的 cmsis_dap.c 按 `(packet_size - 4) / 5`
 * 计算一条命令能带多少个写传输，且不把结果收敛到 255：上报值超过
 * 4 + 255 × 5 = 1279 时，算出的传输数会溢出那个单字节字段（291 被截断成 35），
 * 而它随后仍按未截断的原值校验响应，直接报 "transfer count mismatch" 并让整条
 * 命令流错位。留一个传输的余量取 1274，代价是单条命令少带一个写传输。
 *
 * 本值低于无线单帧能承载的 MAX_DAP_PAYLOAD_SIZE，不会反过来压窄命令上限。
 */
inline constexpr size_t DAP_REPORTED_PACKET_SIZE_MAX = 1274;

/**
 * 计算一条 CMSIS-DAP 请求的完整字节数（含命令 ID）。
 *
 * USB bulk 端点小于命令本身时，一条命令会跨多个 USB 包到达；接收侧必须
 * 按命令自描述长度把字节流重新切分成完整命令，否则半条命令会被当成独立
 * 请求执行。本函数只解析长度：不访问目标、不读写 DAP 状态，可安全用于
 * 收包上下文。
 *
 * @param request 已收到的字节流起始位置
 * @param avail   已收到的字节数
 * @return 完整命令的字节数；数据不足时返回 0（调用方继续累积）
 */
size_t dap_request_size(const u8 *request, size_t avail);

/**
 * 计算一条 CMSIS-DAP 请求可能写入的最大响应字节数（上界）。
 *
 * CMSIS-DAP 的 DAP_ExecuteCommand 写响应不检查缓冲区边界，而若干命令的
 * 响应量随请求线性增长（TransferBlock 的 Count 是 16 位；Sequence 类的
 * 每个捕获序列只占请求 1 字节却能产出最多 8 字节响应；0x7F 批量命令把
 * 子命令响应拼接累加）。执行前用本函数确认响应缓冲装得下，否则会写坏
 * 相邻内存。上界按保守口径计算（例如 Transfer 的读传输一律按带时间戳
 * 的 8 字节计），宁可拒绝异常请求也不冒越界风险。
 *
 * @param request 命令字节流起始位置
 * @param avail   可读字节数
 * @return 响应字节数上界；数据不足时返回 0
 */
size_t dap_response_bound(const u8 *request, size_t avail);
} // namespace mlink

#endif
