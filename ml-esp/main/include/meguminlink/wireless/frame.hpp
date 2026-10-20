#pragma once
#ifndef __MEGUMINLINK_WIRELESS_FRAME_HPP__
#define __MEGUMINLINK_WIRELESS_FRAME_HPP__

#include "esp_err.h"
#include "esp_now.h"
#include "meguminlink/common.hpp" // IWYU pragma: keep
#include "meguminlink/dap_executor.hpp"
#include "meguminlink/wireless/arq_link.hpp" // IWYU pragma: export
#include "sdkconfig.h"

#include <cstring>

namespace mlink
{
/**
 * 业务包：ARQ 帧的载荷。它自己带一个类型字节与各类型自己的帧体，封装成 ARQ 帧
 * （8 字节头 + 本包）由 arq_link 负责——可靠性、序号、确认都是 ARQ 层的事，业务
 * 包只管"这条消息是什么"。
 *
 * 帧格式：[type:1B][帧体]；DapCommand 的帧体是长度 + 通道标记 + 请求号，DapResponse
 * 的是长度 + 请求号。请求号由主机侧分配、接收端原样带回，与 ARQ 序号无关，配对只
 * 认它。数据段上限由 ESP-NOW v2 单帧上限（1470B）扣掉 ARQ 头、本包的类型/长度字段
 * 与最宽的附加字段（命令 3B）得出，当前为 1456B。USB 侧命令按 DAP_PACKET_SIZE 分包
 * 到达，接收侧由 dap_request_size() 重组后再转发。
 */
inline constexpr size_t DAP_FRAME_HEADER_SIZE = 3;
/** DapCommand 帧体里的通道标记字节（见 payload::DapCommand::channel） */
inline constexpr size_t DAP_CHANNEL_FIELD_SIZE = 1;
/** 命令与响应帧体里的请求号字段（见 payload::DapCommand/DapResponse::req_id） */
inline constexpr size_t DAP_REQUEST_FIELD_SIZE = 2;
/** 帧体附加字段的最大宽度：命令带 channel + req_id，响应只带 req_id */
inline constexpr size_t DAP_FRAME_FIELDS_MAX = DAP_CHANNEL_FIELD_SIZE + DAP_REQUEST_FIELD_SIZE;
inline constexpr size_t MAX_DAP_PAYLOAD_SIZE =
    ESP_NOW_MAX_DATA_LEN_V2 - ARQ_HEADER_SIZE - DAP_FRAME_HEADER_SIZE - DAP_FRAME_FIELDS_MAX;
inline constexpr size_t DAP_FRAME_MAX_SIZE =
    ARQ_HEADER_SIZE + DAP_FRAME_HEADER_SIZE + DAP_FRAME_FIELDS_MAX + MAX_DAP_PAYLOAD_SIZE;

/** 执行器请求缓冲（DAP_MAX_REQUEST_SIZE）必须装得下无线单帧能承载的命令 */
static_assert(MAX_DAP_PAYLOAD_SIZE <= DAP_MAX_REQUEST_SIZE, "wire payload exceeds executor request buffer");
static_assert(DAP_FRAME_MAX_SIZE <= ESP_NOW_MAX_DATA_LEN_V2, "frame exceeds ESP-NOW v2 single-frame capacity");
/** 业务包必须装得进 ARQ 层的单条载荷上限 */
static_assert(DAP_FRAME_HEADER_SIZE + DAP_FRAME_FIELDS_MAX + MAX_DAP_PAYLOAD_SIZE <= ArqLink::MAX_PAYLOAD,
              "wire payload exceeds ARQ payload capacity");

/**
 * UartData 帧格式与 DAP 帧一致：[type:1B][len:2B 大端][data:len B]。
 * payload 上限取 200B 而不是顶满单帧上限：串口是流式数据，帧越大越容易被整帧
 * 丢弃（串口通路不重传），200B 在 115200 波特率下约 58 帧/秒，突发日志余量远
 * 大于 64B 单包语义的 DAP 帧。
 */
inline constexpr size_t UART_FRAME_HEADER_SIZE = 3;
inline constexpr size_t MAX_UART_PAYLOAD_SIZE = 200;

namespace payload
{
struct Probe
{
};

struct PairingRequest
{
    u8 target_mac[ESP_NOW_ETH_ALEN];
};

struct PairingAccepted
{
    u8 target_mac[ESP_NOW_ETH_ALEN];
};

/**
 * DAP 命令/响应为"指针视图"：Packet 只是序列化包装层，不内嵌数据。
 * 发送时 data 指向调用方命令/响应缓冲；接收（反序列化）时由调用方提供
 * 存储，data 指向该存储。数据段上限 MAX_DAP_PAYLOAD_SIZE，Packet 自身因此
 * 保持小巧，可放心放任务栈上。
 */
struct DapCommand
{
    u16 req_id;     /**< 主机侧为这条命令分配的请求号：响应原样带回，用于精确配对 */
    u16 len;        /**< 有效数据长度（data[0..len-1]） */
    u8 channel;     /**< 主机侧 USB 传输通道（DAP_CHANNEL_*）：对端据此回答 Packet Size */
    const u8 *data; /**< 命令字节流（指向调用方存储） */
};

/** 与 DapCommand 同构：接收端执行 DAP 命令后的响应视图 */
struct DapResponse
{
    u16 req_id;     /**< 回应的是哪条命令（命令的请求号） */
    u16 len;        /**< 有效数据长度（data[0..len-1]） */
    const u8 *data; /**< 响应字节流（指向调用方存储） */
};

/**
 * 无线转发的串口数据（尽力而为，不重传）：PC 侧 CDC ↔ 目标侧 UART 双向。
 * 数据方向由帧类型承载（UartFromHost / UartFromTarget），接收方据此决定写本机
 * UART 还是本机 CDC，两端因此不必约定谁是 PC 侧、谁是目标侧。数据段上限
 * MAX_UART_PAYLOAD_SIZE = 200B，与 DapCommand 同为指针视图。
 */
struct UartData
{
    u16 len;        /**< 有效数据长度（data[0..len-1]） */
    const u8 *data; /**< 串口字节流（指向调用方存储） */
};

/**
 * 无线同步的 UART 线编码参数（PC 端 CDC SET_LINE_CODING → 目标侧 UART）。
 * 帧数据段 7B：baud 4B 大端 + data_bits/stop_bits/parity 各 1B。参数传 CDC
 * 枚举原值，目标侧翻译为 UART 驱动参数（目标侧无 tinyusb 头，按数值判断）。
 */
struct UartLineCoding
{
    u32 baud;     /**< 波特率（0 = 无偏好，跳过） */
    u8 data_bits; /**< CDC 枚举：5/6/7/8 */
    u8 stop_bits; /**< CDC 枚举：0=1, 1=1.5, 2=2 */
    u8 parity;    /**< CDC 枚举：0=None, 1=Odd, 2=Even, 3=Mark, 4=Space */
};

/**
 * 对端存活探测帧（Ping 与 Pong 同构）。token 由探测方生成、对端原样回显：
 * 探测方据此把迟到的回显（上一轮已判超时之后才到达）与本轮区分开，避免把
 * 陈旧往返算进当前 RTT。
 */
struct Ping
{
    u32 token;
};

struct Pong
{
    u32 token;
};

/**
 * 转发到对端执行的 USB 控制请求（USB SETUP 包原样 + DATA OUT 载荷）。
 *
 * 主机只能直连 USB 侧，无 USB 侧（接收端）的设置只能靠这条通路下发：发送端
 * 把控制请求原样打包给对端，对端用同一张命令表执行，两端语义因此不会漂移，
 * 新增设置项也不需要新开一个 PacketType。
 *
 * 数据段：[len:2 大端][bmRequestType:1][bRequest:1][wValue:2 大端]
 * [wIndex:2 大端][wLength:2 大端][载荷:len B]。len 是载荷字节数，wLength
 * 保持 USB 语义原样透传（OUT 请求为载荷长度、IN 请求为期望响应长度，
 * 两者的区别由对端的命令表决定）。载荷为指针视图：data 指向调用方缓冲，
 * 零拷贝。
 */
struct RelayedUsbControl
{
    u16 len; /**< 载荷字节数（data[0..len-1]） */
    u8 bm_request_type;
    u8 b_request;
    u16 w_value;
    u16 w_index;
    u16 w_length;
    const u8 *data;
};

/** RelayedUsbControl 的固定部分长度（SETUP 包 8B） */
inline constexpr size_t RELAYED_CONTROL_HEADER_SIZE = 8;
/** RelayedUsbControl 可携带的载荷上限（覆盖配置通道现有 OUT 命令的载荷） */
inline constexpr size_t MAX_RELAYED_CONTROL_PAYLOAD = 256;
} // namespace payload

/**
 * 串口数据的来源端口。帧类型由它决定（UartFromHost / UartFromTarget），接收方
 * 按同一方向把数据写进对侧端口：来自主机的写本机 UART TX，来自目标板的写本机
 * CDC。两端因此不需要约定谁是 PC 侧、谁是目标侧。
 */
enum class UartSource : u8
{
    Host,   /**< 来自主机侧 USB CDC */
    Target, /**< 来自目标侧 UART */
};

enum class PacketType
{
    Probe,
    PairingRequest,  /** 配对请求，对方必须回应一个`PairingAccepted`才能完成配对 */
    PairingAccepted, /** 接受配对请求，作为对`PairingRequest`的回应 */
    DapCommand,      /** 无线转发的 DAP 命令载荷（USB 收到的 CMSIS-DAP 请求原样打包） */
    DapResponse,     /** 无线回传的 DAP 响应载荷（接收端执行 DAP 命令的结果原样打包） */
    UartFromHost,    /** 串口数据载荷：来源是主机侧 CDC，接收方写本机 UART TX */
    UartFromTarget,  /** 串口数据载荷：来源是目标侧 UART，接收方写本机 CDC */
    UartLineCoding,  /** 无线同步的 UART 线编码载荷（PC 端 SET_LINE_CODING → 目标侧） */
    Ping,            /** 对端存活探测（定向单播给已配对 peer，对端回 Pong） */
    Pong,            /** 存活探测回显（token 原样带回，探测方据此结算 RTT） */
    RelayedUsbControl, /** 转发到对端执行的 USB 控制请求（无 USB 侧的设置下发通路） */
};

union PacketPayload {
    payload::Probe probe;
    payload::PairingRequest pairing_request;
    payload::PairingAccepted pairing_accepted;
    payload::DapCommand dap_command;
    payload::DapResponse dap_response;
    payload::UartData uart_data;
    payload::UartLineCoding uart_line_coding;
    payload::Ping ping;
    payload::Pong pong;
    payload::RelayedUsbControl relayed_usb_control;
};

class Packet
{
  public:
    Packet();

    /**
     * 从业务包字节反序列化（不含 ARQ 头：那部分由 arq_link 处理）。
     * @param storage      payload 数据存储（DapCommand/DapResponse 的数据
     *                     拷贝到这里，其 data 指针指向本存储）
     * @param storage_cap  存储容量（≥ MAX_DAP_PAYLOAD_SIZE）
     */
    esp_err_t deserialize(const u8 *data, size_t len, u8 *storage, size_t storage_cap);

    /** 序列化成业务包字节（不含 ARQ 头） */
    esp_err_t serialize(u8 *buffer, size_t len) const;

    /** 序列化后的业务包长度 */
    size_t serialized_size() const;

    PacketType packet_type;
    PacketPayload payload;
};

/**
 * 一个已接收的业务包，附带 ESP-NOW 源 MAC。
 * storage 为反序列化的 payload 数据存储（命令/响应字节流），
 * 生命周期与本对象一致（主循环消费完才销毁）。
 * 注意：本对象会被值拷贝入接收队列（vector），拷贝时必须把
 * payload 的 data 指针重绑到拷贝后的 storage（见 rebind_data_ptr）。
 */
struct ReceivedPacket
{
    Packet packet;
    u8 src_mac[ESP_NOW_ETH_ALEN];
    // 入队路径的数据段存储：配对控制帧载荷最多 6B（MAC），按 UartData 上限
    // 预留即可覆盖全部入队类型。
    u8 storage[MAX_UART_PAYLOAD_SIZE];

    ReceivedPacket() = default;

    ReceivedPacket(const ReceivedPacket &other)
    {
        packet = other.packet;
        std::memcpy(src_mac, other.src_mac, ESP_NOW_ETH_ALEN);
        std::memcpy(storage, other.storage, sizeof(storage));
        rebind_data_ptr();
    }

    ReceivedPacket &operator=(const ReceivedPacket &other)
    {
        if (this != &other)
        {
            packet = other.packet;
            std::memcpy(src_mac, other.src_mac, ESP_NOW_ETH_ALEN);
            std::memcpy(storage, other.storage, sizeof(storage));
            rebind_data_ptr();
        }
        return *this;
    }

  private:
    /** 反序列化后 payload 数据总在 storage[0..len)，拷贝后重绑指针 */
    void rebind_data_ptr()
    {
        switch (packet.packet_type)
        {
        case PacketType::DapCommand:
            packet.payload.dap_command.data = storage;
            break;
        case PacketType::DapResponse:
            packet.payload.dap_response.data = storage;
            break;
        case PacketType::UartFromHost:
        case PacketType::UartFromTarget:
            packet.payload.uart_data.data = storage;
            break;
        case PacketType::RelayedUsbControl:
            packet.payload.relayed_usb_control.data = storage;
            break;
        default:
            break;
        }
    }
};

} // namespace mlink

#endif
