#pragma once
#ifndef __MEGUMINLINK_DAP_EXECUTOR_HPP__
#define __MEGUMINLINK_DAP_EXECUTOR_HPP__

#include "meguminlink/common.hpp" // u8/u16

#include <cstddef> // size_t

namespace mlink
{
/**
 * DAP 命令缓冲建议容量：无线帧单帧可承载的请求上限（ESP-NOW v2 单帧
 * 1470B 减去 3B 帧头 = 1467B）。USB bulk 端点仍按 DAP_PACKET_SIZE 分包
 * 到达，接收侧由 dap_request_size() 按命令自描述长度重组为完整命令。
 */
inline constexpr size_t DAP_MAX_REQUEST_SIZE = 1467;

/**
 * DAP 响应缓冲建议容量：0x7F 批量命令把多条子命令的响应拼接在一起，
 * 上限由子命令构成决定（CMSIS-DAP 的 DAP_ExecuteCommand 批量响应写入不
 * 检查边界），此值覆盖常规批量响应。单条命令的响应量级见
 * DAP_MAX_REQUEST_SIZE。
 */
inline constexpr size_t DAP_MAX_RESPONSE_SIZE = 2048;

/**
 * 阻塞获取一条 DAP 命令的源。
 * @param buf     命令缓冲区（容量 buf_cap）
 * @param buf_cap 缓冲区容量（≥ DAP_MAX_REQUEST_SIZE）
 * @param len     输出：命令字节数
 * @param arg     来源自定义上下文
 * @return true=取到命令；false=无命令（继续等待/重试）
 */
using DapCommandSource = bool (*)(u8 *buf, size_t buf_cap, u16 *len, void *arg);

/**
 * 响应输出宿：将执行结果（data/len）回传给调用方（USB 写回 / 无线回发）。
 * @param arg 来源自定义上下文
 */
using DapResponseSink = void (*)(const u8 *data, u16 len, void *arg);

/**
 * 单次执行一条 DAP 命令（DAP_ExecuteCommand + 统一日志），返回响应长度。
 * @param request      原样命令字节流（≤ DAP_MAX_REQUEST_SIZE）
 * @param req_len      命令长度（日志用；CMSIS-DAP 命令自描述）
 * @param response     响应缓冲区（容量 response_cap，≥ DAP_MAX_RESPONSE_SIZE）
 * @param response_cap 响应缓冲区容量
 * @return 响应字节数；调用方负责按需截断/发送。
 */
u16 dap_execute_once(const u8 *request, u16 req_len, u8 *response, size_t response_cap);

/**
 * 通用 DAP 执行器任务主体：DAP_Setup() + 循环
 * （取命令 → dap_execute_once → 响应回传），本函数不返回。
 *
 * 供所有输入源复用：
 * - 无线接收端（命令来自无线队列，响应无线回发，见 wireless_link.cc）；
 * - USB 本地执行（命令来自 USB ringbuffer，响应 USB 写回，见 usb.cc）。
 *
 * 调用方负责以高优先级创建承载任务（SWD 位碰撞执行期间不可被抢占），
 * 并提供命令缓冲（容量 request_cap ≥ DAP_MAX_REQUEST_SIZE）与响应缓冲
 * （容量 response_cap ≥ DAP_MAX_RESPONSE_SIZE）——缓冲放在调用方存储中
 * 而非本函数栈上（ESP-IDF 任务栈以字节计且有限）。
 *
 * @param cmd_src      命令源（阻塞取命令；返回 true=执行，false=不执行）
 * @param resp_sink    响应宿（执行结果回传）
 * @param arg          回调上下文
 * @param request      命令缓冲（cmd_src 写入，容量 request_cap）
 * @param request_cap  命令缓冲容量
 * @param response     响应缓冲（容量 response_cap）
 * @param response_cap 响应缓冲容量
 */
void dap_executor_run(DapCommandSource cmd_src, DapResponseSink resp_sink, void *arg, u8 *request, size_t request_cap,
                      u8 *response, size_t response_cap);
} // namespace mlink

#endif
