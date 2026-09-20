#pragma once
#ifndef __MEGUMINLINK_WIRELESS_FRAME_HPP__
#define __MEGUMINLINK_WIRELESS_FRAME_HPP__

#include "esp_err.h"
#include "esp_now.h"
#include "meguminlink/common.hpp" // IWYU pragma: keep
#include "meguminlink/dap_executor.hpp"
#include "sdkconfig.h"

#if CONFIG_MEGUMINLINK_WIRELESS_ARQ_ENABLE
#include "meguminlink/wireless/arq_link.hpp"
#endif

#include <cstring>

namespace mlink
{
/**
 * 可靠传输扩展头：ARQ 打开时所有帧统一携带，位置固定在 3B 公共帧头（type +
 * len）之后、帧体之前，字段一律大端。它同时承载两个方向的确认信息，因此任意
 * 帧（含 Ping、串口数据、控制帧）都能捎带确认，独立的纯确认帧只在确实没有数据
 * 可捎带时才发。
 *
 * 关闭时长度取 0，帧格式与既有版本逐字节一致——这就是本开关作为回退路径的意义：
 * 两端必须同时刷新且开关一致，否则帧格式不同、完全不通。
 */
#if CONFIG_MEGUMINLINK_WIRELESS_ARQ_ENABLE
inline constexpr size_t ARQ_HEADER_SIZE = 8;

/** 扩展头 flags 位 */
inline constexpr u8 ARQ_FLAG_RELIABLE = 0x01;   /**< seq 字段有效（本帧参与可靠传输） */
inline constexpr u8 ARQ_FLAG_RETRANSMIT = 0x02; /**< 本帧是重传 */
inline constexpr u8 ARQ_FLAG_GAP_BEFORE = 0x04; /**< 本序号之前的空洞已被发送端放弃 */

struct ArqHeader
{
    u16 seq;   /**< 本帧序号（ARQ_FLAG_RELIABLE 未置位时无意义） */
    u16 ack;   /**< 累积确认号：已连续收到 < ack 的全部帧 */
    u8 window; /**< 本端接收窗口通告（0 = 请对端暂停发送） */
    u8 flags;  /**< ARQ_FLAG_* */
    u16 sack;  /**< 选择确认位图：第 i 位对应 seq == ack + 1 + i */
};
#else
inline constexpr size_t ARQ_HEADER_SIZE = 0;
#endif

/**
 * DapCommand/DapResponse 帧格式：[type:1B][扩展头][len:2B 大端][data:len B]。
 * 数据段上限由 ESP-NOW v2 单帧上限（1470B）扣掉公共帧头与扩展头得出，当前为
 * 1459B；ARQ 关闭（扩展头为 0）时回到 1467B。USB 侧命令按 DAP_PACKET_SIZE 分包
 * 到达，接收侧由 dap_request_size() 重组为完整命令后再转发。
 */
inline constexpr size_t DAP_FRAME_HEADER_SIZE = 3;
inline constexpr size_t MAX_DAP_PAYLOAD_SIZE = ESP_NOW_MAX_DATA_LEN_V2 - DAP_FRAME_HEADER_SIZE - ARQ_HEADER_SIZE;
inline constexpr size_t DAP_FRAME_MAX_SIZE = DAP_FRAME_HEADER_SIZE + ARQ_HEADER_SIZE + MAX_DAP_PAYLOAD_SIZE;

/** 执行器请求缓冲（DAP_MAX_REQUEST_SIZE）必须装得下无线单帧能承载的命令 */
static_assert(MAX_DAP_PAYLOAD_SIZE <= DAP_MAX_REQUEST_SIZE, "wire payload exceeds executor request buffer");
static_assert(DAP_FRAME_MAX_SIZE <= ESP_NOW_MAX_DATA_LEN_V2, "frame exceeds ESP-NOW v2 single-frame capacity");

/**
 * UartData 帧格式与 DAP 帧一致：[type:1B][扩展头][len:2B 大端][data:len B]。
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
 * 存储，data 指向该存储。数据段上限 MAX_DAP_PAYLOAD_SIZE（ARQ 打开时 1459B），
 * Packet 自身因此保持小巧，可放心放任务栈上。
 */
struct DapCommand
{
    u16 len;        /**< 有效数据长度（data[0..len-1]） */
    const u8 *data; /**< 命令字节流（指向调用方存储） */
};

/** 与 DapCommand 同构：接收端执行 DAP 命令后的响应视图 */
struct DapResponse
{
    u16 len;        /**< 有效数据长度（data[0..len-1]） */
    const u8 *data; /**< 响应字节流（指向调用方存储） */
};

/**
 * 无线转发的串口数据（尽力而为，不重传）：PC 侧 CDC ↔ 目标侧 UART 双向。
 * 角色天然区分数据方向：发送端（USB 侧）把 CDC RX 打包发送，接收端
 * （无 USB 侧）把收到的帧写入 UART TX；反向同理。数据段上限
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

#if CONFIG_MEGUMINLINK_WIRELESS_ARQ_ENABLE
/**
 * ARQ 会话重置请求：把对端的可靠传输会话拉回同一个序号基准。
 *
 * 序号空间是会话内的相对量，两端必须从同一基准起步。两端一起上电自然对齐，
 * 但只重启一端时不会——重启端从序号初值开始发，对端的交付游标还停在旧位置，
 * 于是新帧全被判成重复帧丢弃，而对端回报的累积确认号又会把它们"确认"掉，
 * 消息静默消失（现象是无线通路突然什么都不通，却没有任何报错）。这条帧就是
 * 让双方重新对齐的手段。
 *
 * epoch 每次上电与会话重置随机生成，只接受携带不同 epoch 的重置请求，同 epoch
 * 的重复帧直接忽略：没有这道判断，一个迟到的旧重置帧会把正在跑的会话打回去，
 * 有了它，旧 epoch 的重置天然被挡住。
 */
/**
 * ArqReset 帧体标志（16 位位域，字段与线格式一一对应）。
 *
 * 请求与回显必须分开：收到请求要回一个回显，收到回显不能再回。少了这道区分，
 * 两端会把对方的回显当成新的重置请求，互相回显下去永不停止。
 */
struct ArqResetFlags
{
    u16 echo : 1;      /**< 本帧是对端重置请求的回显（而不是请求本身） */
    u16 reserved : 15; /**< 预留 */

    /** 线格式写出的 16 位值 */
    u16 raw() const
    {
        return echo ? 0x0001u : 0x0000u;
    }

    /** 由线格式的 16 位值解析（预留位忽略） */
    static ArqResetFlags from_raw(u16 value)
    {
        ArqResetFlags flags = {};
        flags.echo = value & 0x0001u;
        return flags;
    }
};

struct ArqReset
{
    u32 epoch;
    ArqSettings settings; /**< 本端参数：对端 epoch 变化时一并评估采纳 */
    ArqResetFlags flags;  /**< 请求 / 回显 的区分 */
};
#endif
} // namespace payload

#if CONFIG_MEGUMINLINK_WIRELESS_ARQ_ENABLE
/**
 * ArqReset 数据段长度：epoch 4B + 代数 4B + enable/window/max_retx/breaker 4B
 * + 五个 u16 参数 10B + 标志 2B。会话重置帧不带 len 字段（与 Probe 同形），长度
 * 由本常量与收发两侧共同保证。
 */
inline constexpr size_t ARQ_RESET_PAYLOAD_SIZE = 4 + 4 + 4 + 10 + 2;
#endif

enum class PacketType
{
    Probe,
    PairingRequest,  /** 配对请求，对方必须回应一个`PairingAccepted`才能完成配对 */
    PairingAccepted, /** 接受配对请求，作为对`PairingRequest`的回应 */
    DapCommand,      /** 无线转发的 DAP 命令载荷（USB 收到的 CMSIS-DAP 请求原样打包） */
    DapResponse,     /** 无线回传的 DAP 响应载荷（接收端执行 DAP 命令的结果原样打包） */
    UartData,        /** 无线转发的串口数据载荷（PC↔目标 UART 双向，尽力而为） */
    UartLineCoding,  /** 无线同步的 UART 线编码载荷（PC 端 SET_LINE_CODING → 目标侧） */
    Ping,               /** 对端存活探测（定向单播给已配对 peer，对端回 Pong） */
    Pong,               /** 存活探测回显（token 原样带回，探测方据此结算 RTT） */
    RelayedUsbControl,  /** 转发到对端执行的 USB 控制请求（无 USB 侧的设置下发通路） */
#if CONFIG_MEGUMINLINK_WIRELESS_ARQ_ENABLE
    Ack,      /** 纯确认帧：无帧体，累积确认号与 SACK 位图全在扩展头里 */
    ArqReset, /** ARQ 会话重置请求（携带本端 epoch，见 payload::ArqReset） */
#endif
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
#if CONFIG_MEGUMINLINK_WIRELESS_ARQ_ENABLE
    payload::ArqReset arq_reset;
#endif
};

class Packet
{
  public:
    Packet();

    /**
     * 从无线帧反序列化。
     * @param storage      payload 数据存储（DapCommand/DapResponse 的数据
     *                     拷贝到这里，其 data 指针指向本存储）
     * @param storage_cap  存储容量（≥ MAX_DAP_PAYLOAD_SIZE）
     */
    esp_err_t deserialize(const u8 *data, size_t len, u8 *storage, size_t storage_cap);
    esp_err_t serialize(u8 *buffer, size_t len) const;

    /** 序列化后的帧长度，用于发送时告知 ESP-NOW */
    size_t serialized_size() const;

    PacketType packet_type;
    PacketPayload payload;

#if CONFIG_MEGUMINLINK_WIRELESS_ARQ_ENABLE
    /**
     * 可靠传输扩展头。所有帧都带它、与类型无关：非可靠帧（串口数据、控制帧）
     * 的 seq/flags 填 0，但 ack/window/sack 照常有效——它们同样是给对端的一次
     * 确认，这也让"没有数据可发时的确认"能搭上任何一帧。
     */
    ArqHeader arq = {};
#endif
};

/**
 * 一帧已接收的协议包，附带 ESP-NOW 源 MAC。
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
    // 预留即可覆盖全部入队类型。DAP 命令/响应、UartData 与
    // RelayedUsbControl 由 recv_cb 直通处理（不排队），其反序列化落在
    // 调用方提供的大容量缓冲上。
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
        case PacketType::UartData:
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
