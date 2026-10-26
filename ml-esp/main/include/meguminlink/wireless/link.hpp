#pragma once
#ifndef __MEGUMINLINK_WIRELESS_LINK_HPP__
#define __MEGUMINLINK_WIRELESS_LINK_HPP__

#include "esp_err.h"
#include "esp_now.h"  // IWYU pragma: export
#include "esp_wifi.h" // IWYU pragma: export
#include "meaproto/config.hpp"
#include "meaproto/daemon.hpp"
#include "meaproto/lossy_transport.hpp"
#include "meguminlink/common.hpp" // IWYU pragma: keep
#include "meguminlink/dap_executor.hpp"
#include "meguminlink/dap_frame.hpp"
#include "meguminlink/wireless/espnow_transport.hpp"
#include "meguminlink/wireless/frame.hpp"
#include "meguminlink/wireless/uart_egress.hpp"
#include "sdkconfig.h"

#include <atomic>
#include <span>

#include <freertos/FreeRTOS.h> // QueueHandle_t / TaskHandle_t
#include <freertos/queue.h>
#include <freertos/semphr.h> // SemaphoreHandle_t
#include <freertos/task.h>

namespace mlink
{
/**
 * ESP-NOW 发送速率默认档位（11g OFDM 24 Mbps）。
 *
 * 吞吐与档位无关：瓶颈是每条 DAP 命令的往返延迟（~15ms/条）而不是空口时间，
 * 实测从 HT20 MCS6 到 1 Mbps CCK 的写入耗时几乎相同（96KB 负载都在 2.0-2.1s）。
 * 高速档因此不换来任何速度，只剩可靠性差异——高阶调制对信噪比与保护间隔
 * 更敏感，HT20 MCS6/MCS7 在长负载下成片丢帧（MCS7 每命令真丢帧率 ~0.7%），
 * 而本档在 64KB / 96KB 两种负载下全程零丢帧。
 *
 * 取非 HT OFDM 的中间速率而不是 HT20 的某个 MCS：两者实测可靠性相当，但
 * 11g 少了 SGI/LGI 与 HT 前导这些只增加不确定性的旋钮，空口时间又足够短，
 * 不会像低速档那样让长帧在空口上占住太久。
 */
inline constexpr u8 DEFAULT_WIFI_RATE = WIFI_PHY_RATE_24M;

/**
 * 档位是否可用：限 ESP-NOW 可配置的 11b / 11g / HT20 MCS0-7 段
 * （wifi_phy_rate_t 的 0x00-0x1F，其中 0x04 是枚举里的空洞）。
 * 供配置通道在投递动作前校验，避免把非法值送进主循环。
 */
bool wifi_rate_supported(u8 rate);

// ---- 业务通道 ----

/** DAP 命令与响应 */
inline constexpr meaproto::ChannelId DAP_CHANNEL{1};
/** 串口字节与线编码 */
inline constexpr meaproto::ChannelId UART_CHANNEL{2};
/** 配置中继（对端的 EP0 控制请求） */
inline constexpr meaproto::ChannelId CONFIG_CHANNEL{3};

// ---- 串口 ----

/** UART 通道载荷首字节的来源标记 */
inline constexpr u8 UART_SOURCE_HOST = 0;        /**< 来自主机侧 CDC，接收端写本机 UART */
inline constexpr u8 UART_SOURCE_TARGET = 1;      /**< 来自目标侧 UART，接收端写本机 CDC */
inline constexpr u8 UART_SOURCE_LINE_CODING = 2; /**< 线编码，接收端应用到本机 UART */

/** 一条 UART 消息的数据上限：通道上限减去来源标记 */
inline constexpr usize UART_CHUNK = 512 - 1;

/** 线编码载荷：4B 波特率（大端）+ 数据位 + 停止位 + 校验位 */
inline constexpr usize UART_LINE_CODING_SIZE = 7;

/**
 * DAP 通道的单条消息上限。
 *
 * 主机按 DAP_Info 上报的 1274 构造命令，加本层的 kind/source/req_id 头后取 1280：
 * 响应头与批量数据不会把单条消息顶出通道上限。
 */
inline constexpr u16 DAP_CHANNEL_PAYLOAD = 1280;

/**
 * DAP 通道的载荷头：kind（命令/响应）、来源通道与请求号。
 *
 * 来源通道决定接收端回答 DAP_Info 时上报的单包尺寸；请求号由发送端分配、对端
 * 原样带回，响应按它配对。头之后是裸 CMSIS-DAP 命令或响应字节。
 */
inline constexpr usize DAP_WIRE_HEADER = 4;
inline constexpr u8 DAP_KIND_COMMAND = 0;
inline constexpr u8 DAP_KIND_RESPONSE = 1;

/** 单条 DAP 命令/响应的裸字节上限：通道上限减去载荷头 */
inline constexpr usize MAX_DAP_PAYLOAD_SIZE = DAP_CHANNEL_PAYLOAD - DAP_WIRE_HEADER;

/** 无线 DAP 执行队列深度：S3 跟随 USB 命令队列深度，C3 只作接收端，取一半 */
#if CONFIG_IDF_TARGET_ESP32C3
inline constexpr u8 DAP_EXEC_QUEUE_DEPTH = 4;
#else
inline constexpr u8 DAP_EXEC_QUEUE_DEPTH = CONFIG_MEGUMINLINK_PACKET_BUFFER_DEPTH;
#endif

/** 一条待执行的无线 DAP 命令 */
struct DapExecuteRequest
{
    u8 source = 0;  /**< 主机侧 USB 传输通道（DAP_CHANNEL_*） */
    u16 req_id = 0; /**< 请求号：响应原样带回 */
    u16 len = 0;
    u8 data[DAP_CHANNEL_PAYLOAD - DAP_WIRE_HEADER] = {};
};

/**
 * 通道表：同一 id 的上行与下行各自维护序号与窗口，通道之间完全隔离。
 *
 * 优先级数值小的先发：DAP 交互延迟直接决定调试体验，配置中继次之，串口数据最后。
 * 队列与窗口只影响本端资源，不进 config_hash；C3 单核内存紧，按档减半。
 */
#if CONFIG_IDF_TARGET_ESP32C3
inline constexpr meaproto::ChannelSpec CHANNELS[] = {
    {DAP_CHANNEL, meaproto::Reliability::ReliableOrdered, DAP_CHANNEL_PAYLOAD, 4, 1, 4, 4},
    {UART_CHANNEL, meaproto::Reliability::ReliableOrdered, 512, 8, 3, 8, 4},
    {CONFIG_CHANNEL, meaproto::Reliability::ReliableOrdered, 512, 4, 2, 4, 4},
};
#else
inline constexpr meaproto::ChannelSpec CHANNELS[] = {
    {DAP_CHANNEL, meaproto::Reliability::ReliableOrdered, DAP_CHANNEL_PAYLOAD, 8, 1, 8, 8},
    {UART_CHANNEL, meaproto::Reliability::ReliableOrdered, 512, 16, 3, 16, 8},
    {CONFIG_CHANNEL, meaproto::Reliability::ReliableOrdered, 512, 4, 2, 4, 4},
};
#endif

/** 协议参数：窗口、接收队列与重组槽按目标分档（见 meaproto.md §10） */
#if CONFIG_IDF_TARGET_ESP32C3
inline constexpr meaproto::Config LINK_CONFIG = [] {
    meaproto::Config config;
    config.datagram_window = 4;
    config.rx_queue_depth = 4;
    config.max_reassembly_slots = 1;
    return config;
}();
#else
inline constexpr meaproto::Config LINK_CONFIG = [] {
    meaproto::Config config;
    config.datagram_window = 16;
    config.rx_queue_depth = 8;
    config.max_reassembly_slots = 2;
    return config;
}();
#endif

#if CONFIG_MEGUMINLINK_WIRELESS_LOSS_SIM_ENABLE
/** 业务帧的发送路径：注入装饰器包住 ESP-NOW */
using LinkTransport = meaproto::LossyTransport<EspNowTransport>;
#else
using LinkTransport = EspNowTransport;
#endif

#if CONFIG_MEGUMINLINK_WIRELESS_LOSS_SIM_ENABLE
/**
 * 注入丢包率（千分比）：默认不注入，上限 255‰ = 25.5%。
 *
 * 单位取千分比而不是百分比，是为了让 0.1% 这一档有意义：无线 DAP 每条命令
 * 一次往返，0.1% 的丢帧率在 96KB 负载（约 135 条命令）下才刚接近"偶尔失败"，
 * 百分比整数会把整个可用区间压成一个刻度。
 *
 * 参数经配置通道的控制请求下发，那里给命令参数只留了 8 位（见
 * config_channel.cc 的协议注释），255 因此也是通道上限。上端截断不损失测试
 * 能力：2% 已足以让无线 DAP 几乎必然超时。
 */
inline constexpr u8 DEFAULT_LOSS_PERMILLE = 0;
inline constexpr u8 MAX_LOSS_PERMILLE = 255;
#endif

/**
 * 无线链路：ESP-NOW 传输 + peer 管理 + 配对帧分派 + meaproto 连接外壳。
 *
 * 协议连接由 meaproto::Daemon 跑在独立任务上：配对状态变化在 poll() 里收敛成
 * open()/close()，业务消息经 Sink 回到本类。
 *
 * 实例由 Application 持有（m_wireless_link），与 m_usb/m_pairing 同模式。
 */
class WirelessLink : private meaproto::Sink
{
  public:
    WirelessLink();

    /** 初始化传输层并注册回调（幂等） */
    esp_err_t init();

    /**
     * 向指定对端（可为广播地址）发送一帧业务包。
     *
     * 走尽力而为：这条路只承载配对控制帧，它发生在会话建立之前，没有可用的
     * 可靠通道。
     */
    esp_err_t send(const u8 *dest_mac, const Packet &packet);

    /**
     * 把一条 USB 控制请求转发给对端执行（无 USB 侧的设置下发通路）。
     *
     * 走 CONFIG 通道可靠发送；未连接时返回 NotConnected，调用方按"仅本机生效"处理。
     */
    meaproto::SendResult send_relayed_usb_control(const payload::RelayedUsbControl &req);

    /**
     * 提交一条 DAP 命令（发送端，USB 任务上下文）。data 是裸命令字节。
     *
     * 返回 Busy 时调用方应把命令留在 USB 队列里：取出来再丢，主机那条命令就等不到响应。
     */
    meaproto::SendResult send_dap_command(const u8 *data, usize len, u8 source, u16 req_id);

    /** 回一条 DAP 响应（接收端，dap-wireless 任务上下文） */
    meaproto::SendResult send_dap_response(const u8 *data, usize len, u16 req_id);

    /** DAP 通道发送队列是否还有空位（背压用） */
    bool can_send_dap() const;

    /** 连接断开的累计次数：发送端据此判掉在途命令 */
    u32 disconnect_generation() const;

    /**
     * 发一个协议层 Ping 采样往返时延（主循环执行配置通道的 0x18）。
     *
     * 只有已连接才有意义：未连接时返回 false，不生成探测。
     */
    bool ping();

    /**
     * 切换 UART 通道的可靠性（配置通道 0x1D）。两端必须同时切换。
     *
     * 切换会清空该通道的收发状态；未连接时不生效，由连接建立后按落盘值应用。
     * @return true=已切换
     */
    bool set_uart_reliable(bool reliable);

    /** 当前 UART 通道是否走可靠传输 */
    bool uart_reliable() const;

    /** 当前 ESP-NOW 发送速率档位（wifi_phy_rate_t 原值） */
    u8 wifi_rate() const;

    /**
     * 设置 ESP-NOW 发送速率档位（主机经配置通道 0x19 下发）。
     *
     * 档位应用到本地单播 peer，并同步给已配对对端：只改一边会让上行与下行跑在
     * 不同档位，测出的丢包率反映不了任何一个方向的真实情况。同步帧先发、本地
     * 后切，让它走当前（仍然可用的）档位。
     *
     * 档位不做持久化：它是射频环境相关的链路参数而非用户偏好，掉电回默认档可
     * 避免"一端持久化了实验档位、另一端恢复默认"的静默不对称。
     *
     * @param rate wifi_phy_rate_t 原值（wifi_rate_supported() 认可的取值）
     * @return ESP_OK=已应用本地；ESP_ERR_INVALID_ARG=档位不支持
     */
    esp_err_t set_wifi_rate(u8 rate);

#if CONFIG_MEGUMINLINK_WIRELESS_LOSS_SIM_ENABLE
    /** 当前注入丢包率（千分比，0 = 不注入） */
    u8 loss_permille() const;

    /**
     * 设置注入丢包率（主机经配置通道 0x1A 下发，主循环执行）。
     *
     * 与速率档位同样同步给已配对对端：注入只在发送端生效，两端必须各自持有
     * 同一个值，否则只有单方向的帧会被丢，测出来的不是"链路变差"。
     *
     * 不做持久化（同速率档位：射频环境参数而非用户偏好），上电与恢复出厂设置
     * 都回 DEFAULT_LOSS_PERMILLE。
     *
     * @param permille 千分比，> MAX_LOSS_PERMILLE 时返回 ESP_ERR_INVALID_ARG
     */
    esp_err_t set_loss_permille(u8 permille);
#endif

    /**
     * 主循环每拍调用：收敛 peer 登记与应用对端下发的速率档位。
     *
     * 速率变更会操作驱动，不能在交付路径（esp_wifi 任务）里执行，因此接收侧只登记
     * 待生效值，实际应用在这里。
     */
    void poll();

    /** 协议连接状态 */
    bool connected() const;
    bool connecting() const;

    /** 协议层统计快照（配置通道 GetStats 的数据源） */
    meaproto::Stats stats() const;

    /** 协议运行参数（配置通道 GetConfig 的数据源） */
    const meaproto::Config &proto_config() const;

    /** 运行期改时序类参数，见 meaproto::Core::apply_config */
    bool apply_config(const meaproto::Config &config);

    /** DAP 通道的生效发送窗口（0x70 的窗口字段） */
    u8 dap_window() const;

    /**
     * 登记一个对端 peer（encrypt=false）。仅登记尚未存在于 peer list 的地址，
     * 重复调用是安全的。广播地址已由 init() 常驻登记，无需也不应重复登记。
     */
    esp_err_t peer_add(const u8 *peer_mac);

    /** 从 peer list 删除一个对端 peer（配对放弃/完成时归还槽位） */
    esp_err_t peer_remove(const u8 *peer_mac);

    /**
     * 端口数据入出向缓冲（UART 桥任务 / CDC 补读调用）。返回实际入队的字节数：
     * 缓冲满时等空口追上，等不到的部分丢弃，由调用方记账。
     */
    size_t send_uart_data(const u8 *data, size_t len, UartSource source);

    /** 出向缓冲还能装多少字节（调用方据此停读端点） */
    size_t uart_egress_space(UartSource source);

    /** 直接入队（不等待），返回写入字节数 */
    size_t push_uart_egress(const u8 *data, size_t len, UartSource source);

    /** 丢掉两条出向缓冲（断开或重配时清场） */
    void flush_uart_egress();

    /**
     * 排空出向缓冲（主循环每轮调用）：按额度与通道空位逐帧提交，发不出去的留在
     * 缓冲里。
     * @return 仍有积压，调用方应尽快再来一次
     */
    bool poll_uart_egress();

    /** 唤醒主循环排空（端口数据入队后调用） */
    void notify_uart_egress();

    /** 主机 CDC 的补读原语（缓冲腾出空间后由主循环调用） */
    using HostUartFill = void (*)(void *ctx);
    void set_host_uart_fill(HostUartFill fill, void *ctx);

    /** 主机下发的线编码：走 UART 通道，与数据同序 */
    meaproto::SendResult send_line_coding(u32 baud, u8 data_bits, u8 stop_bits, u8 parity);

  private:
    /** Transport 回调（C 函数指针，实例经 ctx 传入） */
    static void on_frame(void *ctx, const u8 *src_mac, const u8 *data, usize len);
    static void on_rx_stats(void *ctx, u16 bytes, i8 rssi);

    void handle_frame(const u8 *src_mac, const u8 *data, usize len);

    /** DAP 通道分派：命令入执行队列，响应交给 USB 侧 */
    void handle_dap_message(std::span<const u8> data);

    /** DAP 载荷组帧并提交（kind + source + req_id + 字节） */
    meaproto::SendResult send_dap_frame(u8 kind, u8 source, u16 req_id, const u8 *data, usize len, u8 *frame);

    /** 无线 DAP 执行任务：取命令、执行、回响应 */
    static void dap_task_fn(void *arg);

    /** UART 通道分派：按来源标记写端口或应用线编码 */
    void handle_uart_message(std::span<const u8> data);

    /** CONFIG 通道分派：解析转发过来的 USB 控制请求 */
    void handle_config_message(std::span<const u8> data);

    /** 串口帧的发送判定 */
    enum class UartVerdict : u8
    {
        Send, /**< 通道有空位且已连接 */
        Wait, /**< 通道队列满或未连接，这一帧留下轮 */
    };

    UartVerdict uart_frame_verdict() const;

    /** 令牌桶准入（Host/Target 分开记账，速率跟随线编码） */
    bool uart_tx_admit(usize len, UartSource source);

    /** 排空一条出向缓冲 */
    void drain_uart_egress(UartEgress &egress, UartSource source);

    UartEgress &uart_egress(UartSource source);

    /** 当前线速换算成字节/秒（未知时用 Kconfig 兜底） */
    u32 uart_bytes_per_s() const;

    /** 把一段已序列化的缓冲交给 ESP-NOW */
    esp_err_t send_frame(const u8 *dest_mac, const u8 *data, size_t len);

    /** 设置单播 peer 的 ESP-NOW 发送速率（广播地址跳过） */
    esp_err_t set_peer_rate(const u8 *peer_mac);

    bool m_initialized = false;
    /** 发送互斥：ESP-NOW 的发送缓冲与驱动调用串行化 */
    SemaphoreHandle_t m_tx_mutex = nullptr;
    /** 业务包序列化缓冲与互斥（多上下文都会调各条发送 helper） */
    SemaphoreHandle_t m_pack_mutex = nullptr;
    u8 m_pack_frame[PACKET_MAX_SIZE] = {};
    /** 链路层业务包的反序列化存储（配对帧与中继包） */
    u8 m_rx_storage[PACKET_MAX_SIZE] = {};
    /** CONFIG 消息的反序列化存储（协议任务独占，不与链路层接收共用） */
    u8 m_config_storage[PACKET_MAX_SIZE] = {};

    /** 无线 DAP：执行队列与任务独立，命令执行不拖住协议任务的 ACK 与保活 */
    QueueHandle_t m_dap_queue = nullptr;
    TaskHandle_t m_dap_task = nullptr;
    DapExecuteRequest m_dap_exec = {};             /**< 执行任务独占的取件缓冲 */
    DapExecuteRequest m_dap_post = {};             /**< 协议任务入队用的拷贝缓冲 */
    u8 m_dap_response[DAP_MAX_RESPONSE_SIZE] = {}; /**< 执行结果 */
    u8 m_dap_cmd_frame[DAP_CHANNEL_PAYLOAD] = {};  /**< 命令组帧（USB 任务） */
    u8 m_dap_rsp_frame[DAP_CHANNEL_PAYLOAD] = {};  /**< 响应组帧（执行任务） */
    /** 超过通道上限被拒的次数 */
    volatile u32 m_dap_too_large = 0;
    /** 断开次数：on_disconnected 递增，发送端比较它来清在途命令表 */
    std::atomic<u32> m_disconnect_generation{0};

    // ---- 串口 ----
    /** 令牌桶容量：两帧 */
    static constexpr u32 UART_BUCKET_CAP_BYTES = 2 * UART_CHUNK;
    /** 判链路忙的窗口：这段时间内提交过 DAP 业务就让串口让路 */
    static constexpr u32 DAP_BUSY_WINDOW_MS = 50;
    /** 出向缓冲满时等待空口追上的上限 */
    static constexpr u32 UART_PUSH_WAIT_MS = 100;
    static constexpr u32 UART_PUSH_WAIT_STEP_MS = 2;

    /** 串口出向令牌桶：桶小才能把突发摊平到毫秒级 */
    struct UartBucket
    {
        portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
        u32 tokens = UART_BUCKET_CAP_BYTES;
        u32 refill_ms = 0;
    };

    UartEgress m_host_uart;          /**< 主机方向出向缓冲 */
    UartEgress m_target_uart;        /**< 目标方向出向缓冲 */
    UartBucket m_uart_bucket_host;   /**< 主机方向额度 */
    UartBucket m_uart_bucket_target; /**< 目标方向额度 */
    /** 当前线编码波特率（0 = 未知）：令牌桶按它换算速率 */
    volatile u32 m_line_baud = 0;
    /** 最近一次提交 DAP 业务包的时刻：串口准入据此给 DAP 让路 */
    volatile u32 m_last_dap_tx_ms = 0;
    /** 主机 CDC 补读原语 */
    HostUartFill m_host_uart_fill = nullptr;
    void *m_host_uart_fill_ctx = nullptr;
    /** 串口组帧缓冲（主循环单线程） */
    u8 m_uart_frame[512] = {};
    /** UART 通道当前是否走可靠传输（协议层状态；落盘值在 NVS） */
    volatile bool m_uart_reliable = true;
    /**
     * 当前 ESP-NOW 发送速率档位。只由主循环写（set_wifi_rate / poll），
     * 读取方（配置通道 GetConfig）在 EP0 上下文。
     */
    volatile u8 m_rate = DEFAULT_WIFI_RATE;
    /**
     * 对端下发的待应用档位：0 = 无待办，否则为 (0x100 | rate)。
     * 打包成单个 u32 并在 m_rate_lock 下读写，使接收回调与主循环之间
     * 既无撕裂也无丢失（新值不会被正在清理旧值的 poll 吞掉）。
     */
    volatile u32 m_pending_rate = 0;
    portMUX_TYPE m_rate_lock = portMUX_INITIALIZER_UNLOCKED;

    // meaproto::Sink
    void on_message(meaproto::ChannelId channel, std::span<const u8> data) override;
    void on_connected() override;
    void on_disconnected(meaproto::DisconnectReason reason) override;

    /** 通道表：本阶段为空 */
    static std::span<const meaproto::ChannelSpec> channels();

    meaproto::Daemon<LinkTransport> m_daemon;
};
} // namespace mlink

#endif
