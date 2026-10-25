#pragma once
#ifndef __MEGUMINLINK_WIRELESS_LINK_HPP__
#define __MEGUMINLINK_WIRELESS_LINK_HPP__

#include "esp_err.h"
#include "esp_now.h"  // IWYU pragma: export
#include "esp_wifi.h" // IWYU pragma: export
#include "meaproto/config.hpp"
#include "meaproto/daemon.hpp"
#include "meguminlink/common.hpp"            // IWYU pragma: keep
#include "meguminlink/wireless/arq_link.hpp" // IWYU pragma: export
#include "meguminlink/wireless/espnow_transport.hpp"
#include "meguminlink/wireless/frame.hpp"
#include "sdkconfig.h"

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

/**
 * DAP 通道的单条消息上限。
 *
 * 主机按 DAP_Info 上报的 1274 构造命令，加本层的 kind/source/req_id 头后取 1280：
 * 响应头与批量数据不会把单条消息顶出通道上限。
 */
inline constexpr u16 DAP_CHANNEL_PAYLOAD = 1280;

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
     * 走尽力而为：这条路是探测、配对与配置通道控制帧的通道，它们要么发生在会话
     * 建立之前，要么本来就不该重传（探活重传会污染 RTT 判定）。
     */
    esp_err_t send(const u8 *dest_mac, const Packet &packet);

    /**
     * 打包一条存活探测包并发送给已配对 peer（Ping/Pong 同构，token 由探测方
     * 生成、对端原样回显）。发送在调用方任务上下文完成（主循环），不在 ESP-NOW
     * 接收回调里回包——回调要短且不可重入驱动。
     */
    esp_err_t send_ping(const u8 *dest_mac, u32 token);
    esp_err_t send_pong(const u8 *dest_mac, u32 token);

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

    /** 会话握手是否已完成（USB 侧据此在转发命令前先等一等） */
    bool arq_established() const;

    /** 可靠传输累计计数（统计与日志用） */
    ArqLink::Snapshot arq_snapshot() const;

    /** 当前可靠传输参数（配置读取与落盘用） */
    ArqSettings arq_settings() const;

    /** 主机下发的可靠传输参数：立即生效并置脏，落盘由主循环做 */
    esp_err_t arq_apply_settings(const ArqSettings &settings);

    /** 参数被改动过（含采纳对端参数）：主循环取走并落盘 */
    bool arq_take_settings_dirty();

    /**
     * 登记一个对端 peer（encrypt=false）。仅登记尚未存在于 peer list 的地址，
     * 重复调用是安全的。广播地址已由 init() 常驻登记，无需也不应重复登记。
     */
    esp_err_t peer_add(const u8 *peer_mac);

    /** 从 peer list 删除一个对端 peer（配对放弃/完成时归还槽位） */
    esp_err_t peer_remove(const u8 *peer_mac);

  private:
    /** Transport 回调（C 函数指针，实例经 ctx 传入） */
    static void on_frame(void *ctx, const u8 *src_mac, const u8 *data, usize len);
    static void on_rx_stats(void *ctx, u16 bytes, i8 rssi);
    static void on_tx_result(void *ctx, const u8 *peer_mac, bool ok);

    void handle_frame(const u8 *src_mac, const u8 *data, usize len);
    void handle_tx_result(bool ok);

    /** 把一段已序列化的缓冲交给 ESP-NOW（退避窗口内直接丢弃） */
    esp_err_t send_frame(const u8 *dest_mac, const u8 *data, size_t len);

    /** 是否处于 ESP-NOW 发送退避窗口内（窗口内只抑制尽力而为帧） */
    bool espnow_backoff_active(u32 now) const;

    /**
     * 抬高退避档位并按档位设定窗口（首档 20ms，逐级翻倍，200ms 封顶）。
     * 触发源有两个：撞上 NO_MEM，以及发送完成回调连续报失败达到阈值。
     */
    void enter_espnow_backoff(u32 now);

    /**
     * 成功发出一帧：若退避窗口已过期则降回正常档位。窗口内的成功不算恢复
     * ——那只是驱动队列碰巧有空位，退避期间确认帧与 DAP 帧本来就在照常尝试。
     */
    void clear_espnow_backoff(u32 now);

    /** 设置单播 peer 的 ESP-NOW 发送速率（广播地址跳过） */
    esp_err_t set_peer_rate(const u8 *peer_mac);

#if CONFIG_MEGUMINLINK_WIRELESS_LOSS_SIM_ENABLE
    /** xorshift32 取下一个随机数（状态在发送锁内推进） */
    u32 next_random();

    /**
     * 本次发送是否按注入丢包率丢弃。
     *
     * 判定、计数与 PRNG 推进都在调用方持有发送锁期间完成，因此不需要额外的
     * 临界区（发送会被 UART 桥任务、USB 任务与主循环调用）。
     */
    bool loss_should_drop();
#endif

    bool m_initialized = false;
    /** 发送互斥：ESP-NOW 的发送缓冲与驱动调用串行化 */
    SemaphoreHandle_t m_tx_mutex = nullptr;
    /** 业务包序列化缓冲与互斥（多上下文都会调各条发送 helper） */
    SemaphoreHandle_t m_pack_mutex = nullptr;
    u8 m_pack_frame[DAP_FRAME_MAX_SIZE] = {};
    /** 接收业务包的反序列化存储：数据段可达 MAX_DAP_PAYLOAD_SIZE，放栈上会吃掉交付任务的栈 */
    u8 m_rx_storage[MAX_DAP_PAYLOAD_SIZE] = {};
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

    /** ESP-NOW 退避：首档 20ms，逐级翻倍，200ms 封顶 */
    static constexpr u16 ESPNOW_BACKOFF_BASE_MS = 20;
    static constexpr u16 ESPNOW_BACKOFF_MAX_MS = 200;
    static constexpr u8 ESPNOW_BACKOFF_MAX_LEVEL = 5;

    /**
     * 发送完成回调连续报失败的阈值：达到即与 NO_MEM 走同一套退避。
     *
     * 连续失败说明空口或对端一侧出了问题（信道被占、对端没有应答），此时继续
     * 满速灌帧只会让驱动队列越堆越深；退避窗口内只抑制尽力而为帧，可靠帧照发。
     */
    static constexpr u8 ESPNOW_TX_FAIL_STREAK_THRESHOLD = 8;
    /** 发送失败日志的限速间隔：差链路下每帧一条会把日志刷满 */
    static constexpr u32 ESPNOW_TX_FAIL_LOG_INTERVAL_MS = 1000;

    /**
     * ESP-NOW 发送退避。撞上 NO_MEM、或发送完成回调连续报失败达到阈值时抬高；
     * 退避窗口过期后仍能发出帧才降回正常；窗口内只抑制串口数据帧，确认帧与
     * DAP 帧照常尝试——队列排空后要靠它们把确认带回去。
     *
     * 档位与截止时刻要一起更新，且 send_raw（多任务）与 send_cb（esp_wifi 任务）
     * 会并发调用，故由 m_espnow_backoff_lock 保护；espnow_backoff_active() 只读
     * 单个截止时刻，保持无锁。
     */
    portMUX_TYPE m_espnow_backoff_lock = portMUX_INITIALIZER_UNLOCKED;
    volatile u32 m_espnow_backoff_until_ms = 0;
    volatile u8 m_espnow_backoff_level = 0;

    /**
     * 发送完成回调报失败的连续次数（成功清零）与最近一条失败日志的时刻。
     * 两者都只由发送结果回调（esp_wifi 任务）读写，不需要额外同步。
     */
    volatile u8 m_send_fail_streak = 0;
    volatile u32 m_send_fail_log_ms = 0;

#if CONFIG_MEGUMINLINK_WIRELESS_LOSS_SIM_ENABLE
    /**
     * 注入丢包率（千分比）。只由主循环写（set_loss_permille），读取方是
     * arq_send_raw 的判定与配置通道 GetConfig（EP0 上下文）——单字节读写，
     * 与 m_rate 同一口径。
     */
    volatile u8 m_loss_permille = DEFAULT_LOSS_PERMILLE;
    /** PRNG 状态：init() 与改档位时用 esp_random 播种，只在发送锁内推进 */
    u32 m_loss_rng = 1;
#endif

    // meaproto::Sink
    void on_message(meaproto::ChannelId channel, std::span<const u8> data) override;
    void on_connected() override;
    void on_disconnected(meaproto::DisconnectReason reason) override;

    /** 通道表：本阶段为空 */
    static std::span<const meaproto::ChannelSpec> channels();

    /** 可靠传输参数的本层副本：供配置通道读写与回显 */
    ArqSettings m_settings{};

    meaproto::Daemon<EspNowTransport> m_daemon;
};
} // namespace mlink

#endif
