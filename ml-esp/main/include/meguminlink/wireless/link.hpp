#pragma once
#ifndef __MEGUMINLINK_WIRELESS_LINK_HPP__
#define __MEGUMINLINK_WIRELESS_LINK_HPP__

#include "esp_err.h"
#include "esp_now.h"                    // IWYU pragma: export
#include "esp_wifi.h"                   // IWYU pragma: export
#include "meguminlink/common.hpp"       // IWYU pragma: keep
#include "meguminlink/wireless/frame.hpp"
#include "sdkconfig.h"

#if CONFIG_MEGUMINLINK_WIRELESS_ARQ_ENABLE
#include "meguminlink/wireless/arq_link.hpp"
#endif

#include <freertos/FreeRTOS.h> // QueueHandle_t / TaskHandle_t
#include <freertos/queue.h>
#include <freertos/semphr.h> // SemaphoreHandle_t
#include <freertos/task.h>

namespace mlink
{
/** ESP-NOW 广播地址，用于发现阶段的 Probe 广播 */
inline constexpr u8 ESP_NOW_BROADCAST[ESP_NOW_ETH_ALEN] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

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

#if CONFIG_MEGUMINLINK_WIRELESS_ARQ_ENABLE
/**
 * 可靠传输消息的类型位：命令与响应走同一条 ARQ 实例，两端的角色（USB 转发侧 /
 * 目标执行侧）由运行时接线决定，帧类型因此随消息走，而不是按本端编译配置一刀切。
 *
 * meta 是 arq 核心原样保存、重传与重放时原样带回的 1 字节（见 arq.hpp 的
 * TransmitFn 与 ReorderStore），最高位在这里作类型标记，低 7 位继续承载主机侧
 * USB 通道标记（DAP_CHANNEL_*，现值只占 0/1）。
 */
inline constexpr u8 ARQ_META_RESPONSE = 0x80;
/** meta 里的通道标记掩码（去掉类型位） */
inline constexpr u8 ARQ_META_CHANNEL_MASK = 0x7F;
#endif

/**
 * 无线链路：ESP-NOW 传输 + peer 管理 + 无线侧 DAP 执行接线，状态全在对象内。
 * 与 usb.cc 的 dap_task（USB 侧执行接线）对称：无线传输层负责把收到的
 * DapCommand 交给公共执行器（dap_executor_run）执行，结果以 DapResponse
 * 帧回发给来源；发送端（USB 侧）经 submit_dap_command 提交命令，响应由接收回调
 * 入队后按可靠传输序号配回。
 * 实例由 Application 持有（m_wireless_link），与 m_usb/m_pairing 同模式。
 */
class WirelessLink
{
  public:
    /** 初始化 WiFi/ESP-NOW、登记广播 peer，并创建无线侧 DAP 执行器（幂等） */
    esp_err_t init();

    /** 向指定对端（可为广播地址）发送一帧协议包 */
    esp_err_t send(const u8 *dest_mac, const Packet &packet);

    /**
     * 打包一条 DAP 命令/响应（原样字节流）为对应帧并发送给对端。命令帧额外带上
     * 主机侧通道标记（channel，见 DAP_CHANNEL_*）：对端据此回答 DAP_Info 的单包
     * 尺寸；响应帧不需要它。
     */
    esp_err_t send_dap_command(const u8 *dest_mac, const u8 *data, size_t len, u8 channel);
    esp_err_t send_dap_response(const u8 *dest_mac, const u8 *data, size_t len);

    /**
     * 打包一段串口数据为 UartData 帧并发送给对端（尽力而为，不重传）。
     * @param len 数据长度（> MAX_UART_PAYLOAD_SIZE 时拒绝）
     */
    esp_err_t send_uart_data(const u8 *dest_mac, const u8 *data, size_t len);

    /**
     * 打包线编码参数为 UartLineCoding 帧并发送给对端（尽力而为，不重传）。
     * 参数为 CDC 枚举原值（见 payload::UartLineCoding），目标侧翻译应用。
     */
    esp_err_t send_uart_line_coding(const u8 *dest_mac, u32 baud, u8 data_bits, u8 stop_bits, u8 parity);

    /**
     * 打包一条存活探测帧并发送给已配对 peer（Ping/Pong 同构，token 由探测方
     * 生成、对端原样回显）。发送在调用方任务上下文完成（主循环），不在 ESP-NOW
     * 接收回调里回包——回调要短且不可重入驱动。
     */
    esp_err_t send_ping(const u8 *dest_mac, u32 token);
    esp_err_t send_pong(const u8 *dest_mac, u32 token);

    /**
     * 把一条 USB 控制请求原样转发给对端执行（无 USB 侧的设置下发通路）。
     * 纯载体：本函数不解释命令内容，对端用自己的配置通道命令表执行。
     * @param req 控制请求（载荷为指针视图，调用期间须保持有效）
     * @return 载荷超过 MAX_RELAYED_CONTROL_PAYLOAD 时返回 ESP_ERR_INVALID_SIZE
     */
    esp_err_t send_relayed_usb_control(const u8 *dest_mac, const payload::RelayedUsbControl &req);

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
     * 主循环每拍调用：应用对端经 RelayedUsbControl 下发的速率档位。
     *
     * 速率变更会操作 WiFi 驱动，不能在 ESP-NOW 接收回调里执行（回调不可重入
     * 驱动），因此接收路径只登记待生效值，实际应用在这里。
     */
    void poll();

#if CONFIG_MEGUMINLINK_WIRELESS_ARQ_ENABLE
    /**
     * 驱动可靠传输：重传、纯确认、响应重放与会话握手。调用方是主循环与 USB 侧
     * 的等待循环——接收回调只置待发标记，所有出向帧都在这里发出。
     */
    void arq_poll(u32 now_ms);

    /** 会话握手是否已完成（USB 侧据此在转发命令前先等一等） */
    bool arq_established() const;

    /** 可靠传输的下一个待办时刻（0 = 无待办），调用方据此收短等待超时 */
    u32 arq_next_deadline_ms() const;

    /** 可靠传输累计计数（统计与日志用） */
    ArqLink::Snapshot arq_snapshot() const;

    /** 当前可靠传输参数（配置读取与落盘用） */
    ArqSettings arq_settings() const;

    /** 主机下发的可靠传输参数：立即生效并置脏，落盘由主循环做 */
    esp_err_t arq_apply_settings(const ArqSettings &settings, u32 now_ms);

    /** 参数被改动过（含采纳对端参数）：主循环取走并落盘 */
    bool arq_take_settings_dirty();
#endif

    /**
     * 异步转发一条 DAP 命令给配对 peer（Wireless 模式 USB→无线通路）：只负责把它
     * 发出去并回填它的可靠传输序号，不等响应。
     *
     * 响应不在这里等——接收回调把它连同序号入 Application 的响应队列，USB 侧的
     * dap_task 再按序号配回本条命令。多条命令因此可以同时在途，窗口才有意义。
     *
     * 装了可靠传输时，提交前会先把会话握手做完（两端序号基准对齐），失败由重传
     * 兜底；重传预算在 MEGUMINLINK_WIRELESS_RESPONSE_TIMEOUT_MS 之内耗尽则判失败，
     * 那之后由 take_failed_command 上抛给调用方销账。
     *
     * @param channel 该命令来自哪条主机侧 USB 通道（DAP_CHANNEL_*）：随命令送到对端，
     *                供它回答 DAP_Info 的单包尺寸
     * @param seq_out 输出：本条命令的可靠传输序号（未启用可靠传输时为 0）
     * @return ESP_OK=已发出；否则失败（未配对 / 会话未对齐 / 发送或提交失败）
     */
    esp_err_t submit_dap_command(const u8 *cmd, size_t len, u8 channel, u16 *seq_out);

    /**
     * 取出一条已被可靠传输判定失败的命令序号（重传耗尽）。
     * 调用方据此把对应的在途条目销账：策略是不回响应，让主机那条命令自己超时。
     * @return false = 当前没有待处理的失败
     */
    bool take_failed_command(u16 *seq_out);

    /**
     * 现在能不能提交新命令（可靠传输窗口还有空位）。
     *
     * USB 侧的 dap_task 用它做背压：不能提交时先不取命令，让命令留在队列里，
     * 主机的 USB 写入自然被挡回；取出来再丢弃会让那条命令永远等不到响应。
     */
    bool can_submit_command();

    /**
     * 登记一个对端 peer（encrypt=false）。仅登记尚未存在于 peer list 的地址，
     * 重复调用是安全的。广播地址已由 init() 常驻登记，无需也不应重复登记。
     */
    esp_err_t peer_add(const u8 *peer_mac);

    /** 从 peer list 删除一个对端 peer（配对放弃/完成时归还槽位） */
    esp_err_t peer_remove(const u8 *peer_mac);

    /**
     * 投递一条无线接收到的 DAP 命令给执行器（异步入队，立即返回）。
     * @param src_mac 命令来源 MAC（响应回发地址，6 字节）
     * @param cmd_seq 该命令在可靠传输里的序号（无 ARQ 时填 0）；回响应时按它记账，
     *                对端重传同一条命令时据此重放响应而不是重新执行
     * @param data    DAP 命令原样字节流
     * @param len     命令长度（> MAX_DAP_PAYLOAD_SIZE 时拒绝）
     * @param channel 该命令来自的主机侧 USB 通道（DAP_CHANNEL_*）：执行器据此回答
     *                DAP_Info 的单包尺寸
     */
    esp_err_t dap_execute(const u8 *src_mac, u16 cmd_seq, const u8 *data, size_t len, u8 channel);

  private:
    /** ESP-NOW 接收/发送回调（esp_now 要求 C 函数指针，无 arg；经 Application 访问） */
    static void recv_cb(const esp_now_recv_info_t *recv_info, const u8 *data, int data_len);
    static void send_cb(const wifi_tx_info_t *send_info, esp_now_send_status_t send_status);

    /** 无线侧 DAP 执行器回调（arg = 本对象 this）与任务入口 */
    static bool dap_cmd_src(u8 *buf, size_t buf_cap, u16 *len, void *arg);
    static void dap_resp_sink(const u8 *data, u16 len, void *arg);
    static void dap_task_fn(void *arg);

    /**
     * 序列化并交给 ESP-NOW。只取发送锁，不碰可靠传输的锁——可靠传输的重传回调
     * 是在持有它自己那把锁的情况下调用本函数的，反过来再取一次会死锁。
     */
    esp_err_t send_raw(const u8 *dest_mac, const Packet &packet);

    /** 设置单播 peer 的 ESP-NOW 发送速率（广播地址跳过） */
    esp_err_t set_peer_rate(const u8 *peer_mac);

#if CONFIG_MEGUMINLINK_WIRELESS_ARQ_ENABLE
    /**
     * 可靠传输的输出回调：把核心给的消息/确认/重置封成帧送出去。
     * meta 的类型位决定本条消息封成命令帧还是响应帧（见 ARQ_META_RESPONSE）。
     */
    static void arq_transmit(void *ctx, u16 seq, const u8 *payload, u16 len, bool retransmit, bool gap_before, u8 meta,
                             u16 ack, u16 sack, u8 window);
    static void arq_send_ack(void *ctx, u16 ack, u16 sack, u8 window);
    static void arq_send_reset(void *ctx, u32 epoch, const ArqSettings &settings, bool echo, u16 ack, u16 sack,
                               u8 window);
    /** 一条按序交付的可靠消息：meta 的类型位决定它交给执行器还是入响应队列 */
    static void arq_deliver(void *ctx, const u8 *peer_mac, u16 seq, const u8 *payload, u16 len, u8 meta);
#endif

#if CONFIG_MEGUMINLINK_WIRELESS_LOSS_SIM_ENABLE
    /** xorshift32 取下一个随机数（状态在 send 的发送锁内推进） */
    u32 next_random();

    /**
     * 本次发送是否按注入丢包率丢弃。
     *
     * 只有 DAP 与串口数据帧参与判定：配对、探测与配置通道控制帧必须永远走得
     * 通，否则丢包率调高之后连"改回来"的控制请求都会一起丢掉。
     *
     * 判定、计数与 PRNG 推进都在调用方持有发送锁期间完成，因此不需要额外的
     * 临界区（send 会被 USB DAP 任务、UART 桥任务、EP0 上下文与主循环调用）。
     */
    bool loss_should_drop(PacketType type);
#endif

    static constexpr u8 CHANNEL = 10; // ESP-NOW 固定信道

    bool m_initialized = false;
    /** 发送帧缓冲与互斥：DAP 帧可达 1470B，放栈上会超出发送方任务的栈 */
    SemaphoreHandle_t m_tx_mutex = nullptr;
    u8 m_tx_frame[DAP_FRAME_MAX_SIZE];
    /** 接收帧反序列化存储：数据段可达 MAX_DAP_PAYLOAD_SIZE，放栈上会吃掉回调任务的栈 */
    u8 m_rx_storage[MAX_DAP_PAYLOAD_SIZE];
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

#if CONFIG_MEGUMINLINK_WIRELESS_LOSS_SIM_ENABLE
    /**
     * 注入丢包率（千分比）。只由主循环写（set_loss_permille），读取方是 send
     * 的判定（各任务上下文）与配置通道 GetConfig（EP0 上下文）——单字节读写，
     * 与 m_rate 同一口径。
     */
    volatile u8 m_loss_permille = DEFAULT_LOSS_PERMILLE;
    /** PRNG 状态：init() 与改档位时用 esp_random 播种，只在发送锁内推进 */
    u32 m_loss_rng = 1;
#endif

    /** 无线侧 DAP 执行器（命令队列 + 任务 + 缓冲，状态全在对象内） */
    QueueHandle_t m_dap_queue = nullptr;
    TaskHandle_t m_dap_task = nullptr;
    /** 最近一条命令的来源（响应回发地址）；仅执行任务线程读写 */
    u8 m_dap_src_mac[ESP_NOW_ETH_ALEN];
    /** 最近一条命令的可靠传输序号；仅执行任务线程读写（回响应时按它记账） */
    u16 m_dap_cmd_seq = 0;
    /** 执行器命令/响应缓冲（不占任务栈，见 dap_executor.hpp） */
    u8 m_dap_request[DAP_MAX_REQUEST_SIZE];
    u8 m_dap_response[DAP_MAX_RESPONSE_SIZE];

#if CONFIG_MEGUMINLINK_WIRELESS_ARQ_ENABLE
    /** 可靠传输适配层（收发状态机 + 会话握手 + 重放排队，见 arq_link.hpp） */
    ArqLink m_arq;
#endif
};
} // namespace mlink

#endif
