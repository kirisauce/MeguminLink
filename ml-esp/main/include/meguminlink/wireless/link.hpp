#pragma once
#ifndef __MEGUMINLINK_WIRELESS_LINK_HPP__
#define __MEGUMINLINK_WIRELESS_LINK_HPP__

#include "esp_err.h"
#include "esp_now.h"                    // IWYU pragma: export
#include "esp_wifi.h"                   // IWYU pragma: export
#include "meguminlink/common.hpp"       // IWYU pragma: keep
#include "meguminlink/wireless/arq_link.hpp" // IWYU pragma: export
#include "meguminlink/wireless/frame.hpp"
#include "sdkconfig.h"

#include <freertos/FreeRTOS.h> // QueueHandle_t / TaskHandle_t
#include <freertos/queue.h>
#include <freertos/semphr.h> // SemaphoreHandle_t
#include <freertos/task.h>

namespace mlink
{
/** ESP-NOW 广播地址，用于发现阶段的 Probe 广播 */
inline constexpr u8 ESP_NOW_BROADCAST[ESP_NOW_ETH_ALEN] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

/**
 * 一条待执行的 DAP 命令：命令来源（响应回发）+ 可靠传输序号 + 原样命令字节流。
 *
 * 约 1.4 KB，而且是"从命令队列取一条命令"的落点——它必须是对象成员，不能是
 * dap-wireless 任务里的局部变量：那条任务只有 4096 字节栈，下面还要跑
 * DAP_ExecuteCommand 的整条调用链。
 */
struct DapExecuteRequest
{
    u8 src_mac[ESP_NOW_ETH_ALEN];
    u16 cmd_seq;
    u16 len;
    u8 channel; /**< 主机侧 USB 传输通道（DAP_CHANNEL_*）：决定回给主机的 Packet Size */
    u8 data[MAX_DAP_PAYLOAD_SIZE];
};

/** 响应型提交的上下文：本条载荷回应的是哪条命令（序号由对端分配） */
struct ResponseContext
{
    bool is_response = false;
    u16 cmd_seq = 0;
};

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

/**
 * 无线链路：ESP-NOW 传输 + peer 管理 + 业务包编解码 + 无线侧 DAP 执行接线，
 * 状态全在对象内。与 usb.cc 的 dap_task（USB 侧执行接线）对称：无线传输层把
 * 收到的 DapCommand 交给公共执行器（dap_executor_run）执行，结果以 DapResponse
 * 业务包回发给来源；发送端（USB 侧）经 submit_dap_command 提交命令，响应由接收
 * 回调入队后按可靠传输序号配回。
 *
 * 可靠性归 arq_link 管：本层只负责"这是什么业务包"，每条发送通路在调用
 * m_arq.submit 时显式给出自己的可靠性等级——本层不按包类型猜。
 * 实例由 Application 持有（m_wireless_link），与 m_usb/m_pairing 同模式。
 */
class WirelessLink
{
  public:
    /** 初始化 WiFi/ESP-NOW、登记广播 peer，并创建无线侧 DAP 执行器（幂等） */
    esp_err_t init();

    /**
     * 向指定对端（可为广播地址）发送一帧业务包。
     *
     * 走不可靠路径：这条路是探测与配对（以及配置通道控制帧）的通道，它们要么发生
     * 在会话建立之前，要么本来就不该重传（探活重传会污染 RTT 判定）。需要保达的
     * 业务各有自己的 helper（DAP、线编码、中继）。
     */
    esp_err_t send(const u8 *dest_mac, const Packet &packet);

    /**
     * 打包一条 DAP 命令/响应（原样字节流）为对应业务包并通过可靠传输发送给对端。
     * 命令包额外带上主机侧通道标记（channel，见 DAP_CHANNEL_*）：对端据此回答
     * DAP_Info 的单包尺寸；响应包不需要它。
     */
    esp_err_t send_dap_command(const u8 *dest_mac, const u8 *data, size_t len, u8 channel, u16 *seq_out = nullptr);

    /**
     * 回发一条 DAP 响应。@param cmd_seq 它回应的是哪条命令：对端重传那条命令时
     * 据此重放这条响应，而不是把命令再执行一遍。
     */
    esp_err_t send_dap_response(const u8 *dest_mac, const u8 *data, size_t len, u16 cmd_seq);

    /**
     * 串口出向准入：令牌桶限速 + DAP 让路。
     *
     * 串口包没有窗口与重传保护，进入空口的速率必须由本端自己管住：超过对端
     * UART 消费能力的部分直接丢弃，否则 ESP-NOW 的发送队列会被灌满，连 DAP
     * 的可靠帧与确认帧都发不出去。DAP 有在途消息时按配置比例压低额度，把
     * 链路让给可靠流量。
     *
     * @param len 待发帧的载荷长度（字节）
     * @return true = 放行；false = 拒绝（已计入 GetStats 的 uart_tx_admit_dropped）
     */
    bool uart_tx_admit(size_t len);

    /**
     * 无线路径：把一段串口数据发给已配对 peer（尽力而为，不重传）。配对地址查询
     * 与出向准入都在这里完成——串口没有窗口与重传保护，超过对端消费能力的部分
     * 直接丢弃，否则 ESP-NOW 的发送队列会被灌满。
     *
     * @param source 数据来自本机哪个端口：UartSource::Host 封成 UartFromHost 包
     *               （对端写它的 UART TX），UartSource::Target 封成 UartFromTarget
     *               包（对端写它的 CDC）
     * @param len 任意长度：超过单帧载荷上限（MAX_UART_PAYLOAD_SIZE）的部分在这里
     *            切成多帧发出，调用方不必自己分块
     */
    esp_err_t send_uart_data(const u8 *data, size_t len, UartSource source);

    /**
     * 无线路径：打包线编码参数为 UartLineCoding 包发给已配对 peer（可靠传输：
     * 丢了参数就静默不跟手，这条通路值得一次重传预算）。
     * 参数为 CDC 枚举原值（见 payload::UartLineCoding），目标侧翻译后应用。
     */
    esp_err_t send_line_coding(u32 baud, u8 data_bits, u8 stop_bits, u8 parity);

    /**
     * 打包一条存活探测包并发送给已配对 peer（Ping/Pong 同构，token 由探测方
     * 生成、对端原样回显）。发送在调用方任务上下文完成（主循环），不在 ESP-NOW
     * 接收回调里回包——回调要短且不可重入驱动。
     */
    esp_err_t send_ping(const u8 *dest_mac, u32 token);
    esp_err_t send_pong(const u8 *dest_mac, u32 token);

    /**
     * 把一条 USB 控制请求原样转发给对端执行（无 USB 侧的设置下发通路）。走可靠
     * 传输：这是一次设置下发，丢了不会有人重来。
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
     * 主循环每拍调用：应用对端经 RelayedUsbControl 下发的速率档位、应用可靠
     * 传输收到的线编码参数，并在链路熔断时打主动探针。
     *
     * 速率变更与 UART 参数都会操作驱动，不能在交付路径（ARQ 任务）里执行，
     * 因此接收侧只登记待生效值，实际应用在这里。
     */
    void poll();

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
     * 异步转发一条 DAP 命令给配对 peer（Wireless 模式 USB→无线通路）：只负责把它
     * 发出去并回填它的可靠传输序号，不等响应。
     *
     * 响应不在这里等——接收回调把它连同序号入 Application 的响应队列，USB 侧的
     * dap_task 再按序号配回本条命令。多条命令因此可以同时在途，窗口才有意义。
     *
     * 提交前会先把会话握手做完（两端序号基准对齐），失败由重传兜底；重传预算在
     * MEGUMINLINK_WIRELESS_RESPONSE_TIMEOUT_MS 之内耗尽则判失败，那之后由
     * take_failed_command 上抛给调用方销账。
     *
     * @param channel 该命令来自哪条主机侧 USB 通道（DAP_CHANNEL_*）：随命令送到对端，
     *                供它回答 DAP_Info 的单包尺寸
     * @param seq_out 输出：本条命令的可靠传输序号
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
     * 现在能不能提交新命令（会话已对齐、窗口还有空位）。
     *
     * USB 侧的 dap_task 用它做背压：不能提交时先不取命令，让命令留在队列里，
     * 主机的 USB 写入自然被挡回；取出来再丢弃会让那条命令永远等不到响应。
     */
    bool can_submit_command();

    /**
     * 链路被证明可用（收到对端 Pong 一类回显）：请可靠传输清掉熔断与连续失败计数。
     * 调用方是主循环消费 Pong 帧的分支。
     */
    void notify_link_alive();

    /** 执行队列满被丢弃的命令数（接收侧跟不上时才会增长） */
    u32 cmd_queue_dropped() const;

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
     * @param cmd_seq 该命令在可靠传输里的序号；回响应时按它记账，
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

    /** ARQ 层的裸发送回调：只把这段缓冲交给 ESP-NOW，不认识内容 */
    static void arq_send_raw(void *ctx, const u8 *dest_mac, const u8 *data, size_t len, bool reliable,
                             bool loss_injectable);

    /** ARQ 层的交付回调：解析业务包并按类型分派（跑在 ARQ 任务上下文） */
    static void arq_deliver(void *ctx, const u8 *src_mac, u16 seq, const u8 *payload, u16 len);

    /**
     * 把一段缓冲发给 ESP-NOW。只取发送锁，不碰 ARQ 状态——本函数的调用方（ARQ
     * 任务）正是 ARQ 状态的持有者，反过来再取会死锁。
     */
    esp_err_t send_raw(const u8 *dest_mac, const u8 *data, size_t len, bool reliable, bool loss_injectable);

    /**
     * 序列化一个业务包并交给 ARQ 层（命令与响应共用这一条出口）。序列化缓冲是
     * 对象成员，多上下文调用由 m_pack_mutex 串起来（提交本身是异步入环，锁内
     * 不做等待）。
     */
    esp_err_t submit_packet(const u8 *dest_mac, const Packet &packet, ArqLink::Reliability reliability,
                            bool loss_injectable, u16 *seq_out = nullptr, const ResponseContext &response = ResponseContext{});

    /** 是否处于 ESP-NOW 发送退避窗口内（窗口内只抑制串口数据帧） */
    bool espnow_backoff_active(u32 now) const;

    /** 撞上 NO_MEM：抬高退避档位并按档位设定窗口（首档 20ms，逐级翻倍，200ms 封顶） */
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
     * 临界区（发送会被 ARQ 任务、UART 桥任务与主循环调用）。
     */
    bool loss_should_drop();
#endif

    static constexpr u8 CHANNEL = 10; // ESP-NOW 固定信道

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

    /**
     * 对端下发的待应用线编码参数（ARQ 交付路径只登记，主循环应用——configure 会
     * 调 UART 驱动，不该占用 ARQ 任务的重传定时器）。
     */
    struct PendingLineCoding
    {
        u32 baud;
        u8 data_bits;
        u8 stop_bits;
        u8 parity;
    };

    portMUX_TYPE m_line_coding_lock = portMUX_INITIALIZER_UNLOCKED;
    PendingLineCoding m_pending_line_coding = {};
    bool m_line_coding_pending = false;

    /** 串口出向令牌桶容量：两帧。桶小才能把突发摊平到毫秒级 */
    static constexpr u32 UART_BUCKET_CAP_BYTES = 2 * MAX_UART_PAYLOAD_SIZE;
    /** 判"链路正忙"的窗口：这段时间内发过可靠帧就让串口让路 */
    static constexpr u32 DAP_BUSY_WINDOW_MS = 50;
    /** ESP-NOW 退避：首档 20ms，逐级翻倍，200ms 封顶 */
    static constexpr u16 ESPNOW_BACKOFF_BASE_MS = 20;
    static constexpr u16 ESPNOW_BACKOFF_MAX_MS = 200;
    static constexpr u8 ESPNOW_BACKOFF_MAX_LEVEL = 5;

    /**
     * 串口出向令牌桶。只有串口数据帧过这道闸（DAP 的可靠帧不受限）：桶满两帧、
     * 按对端线速补充，链路正忙时按配置比例压低。调用方是 CDC 回调（tinyusb
     * 任务）与 UART 桥任务，自旋锁保护。
     */
    portMUX_TYPE m_uart_bucket_lock = portMUX_INITIALIZER_UNLOCKED;
    u32 m_uart_bucket_tokens = UART_BUCKET_CAP_BYTES;
    u32 m_uart_bucket_refill_ms = 0;

    /**
     * ESP-NOW 发送退避。撞上 NO_MEM 时抬高，退避窗口过期后仍能发出帧才降回
     * 正常；窗口内只抑制串口数据帧，确认帧与 DAP 帧照常尝试——队列排空后要靠
     * 它们把确认带回去。单字读写在 32 位目标上原子，不需要临界区。
     */
    volatile u32 m_espnow_backoff_until_ms = 0;
    volatile u8 m_espnow_backoff_level = 0;

    /**
     * 最近一次可靠帧（DAP 命令/响应）发出的时刻。串口准入据此判断链路是否正忙：
     * 查 ARQ 快照要取锁，而准入跑在收数据的每一帧上。
     */
    volatile u32 m_last_reliable_tx_ms = 0;

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

    /** 无线侧 DAP 执行器（命令队列 + 任务 + 缓冲，状态全在对象内） */
    QueueHandle_t m_dap_queue = nullptr;
    TaskHandle_t m_dap_task = nullptr;
    /** 最近一条命令的来源（响应回发地址）；仅执行任务线程读写 */
    u8 m_dap_src_mac[ESP_NOW_ETH_ALEN];
    /** 最近一条命令的可靠传输序号；仅执行任务线程读写（回响应时按它记账） */
    u16 m_dap_cmd_seq = 0;
    /** 从命令队列取出的待执行命令（不占 dap-wireless 任务栈；仅执行任务线程读写） */
    DapExecuteRequest m_dap_pending;
    /** 执行器命令/响应缓冲（不占任务栈，见 dap_executor.hpp） */
    u8 m_dap_request[DAP_MAX_REQUEST_SIZE];
    u8 m_dap_response[DAP_MAX_RESPONSE_SIZE];

    /**
     * ARQ 层（任务 + 收发状态机 + 会话握手 + 重放，见 arq_link.hpp）。
     * 它自己持有自己的任务与状态，本层只提交字节与接收交付。
     */
    ArqLink m_arq;

    /**
     * 上一次熔断探针（Ping）的发出时刻。
     *
     * 熔断期间主机可能根本没有命令要提交（空闲），光靠"提交时试探"恢复不了；
     * 主循环按这个时刻周期性地主动打一个探针，把链路状态问回来。
     */
    u32 m_arq_probe_tx_ms = 0;

    /** 执行队列满被丢弃的命令数（接收侧跟不上时才会增长） */
    volatile u32 m_cmd_queue_dropped = 0;
};
} // namespace mlink

#endif
