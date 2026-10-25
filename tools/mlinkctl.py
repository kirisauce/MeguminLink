#!/usr/bin/env python3
"""mlinkctl —— MeguminLink 配置通道的命令行探针。

一次调用 = 一次 EP0 厂商控制请求（bRequest 0x4D），打印一行 key=value 后退出：

    mlinkctl                     读一次统计快照（cmd 0x01 GET_STATS）
    mlinkctl config              读设备信息与配置项（cmd 0x02 GET_CONFIG）
    mlinkctl usb                 读 USB 能力：WebUSB / WinUSB / HID 兼容模式 / landing page / 设备名称（cmd 0x03）
    mlinkctl set-usb [...]       设置 USB 能力与设备名称，保存后设备自动重新枚举（cmd 0x16 + 0x17）
    mlinkctl set-mode <mode>     切换运行模式：local | wireless（cmd 0x10）
    mlinkctl set-rate <rate>     设置 ESP-NOW 发送速率档位，两端同时应用（cmd 0x19）
    mlinkctl set-loss <percent>  设置注入丢包率，两端同时应用（cmd 0x1A，需固件编译该功能）
    mlinkctl set-uart-reliable on|off  串口出向是否走可靠传输，两端同时应用（cmd 0x1D）
    mlinkctl pair                进入配对流程（cmd 0x11）
    mlinkctl exit-pair           退出配对流程（cmd 0x15）
    mlinkctl ping [-c N]         探测已配对对端：ESP-NOW 往返时延（cmd 0x18）
    mlinkctl unpair              清除配对记录并重新配对（cmd 0x12）
    mlinkctl reset               恢复出厂设置：清除配对、模式与 USB 能力记录后重启（cmd 0x13）
    mlinkctl reboot              重启设备（cmd 0x14）
    mlinkctl update             进入更新模式：软复位后只保留一个 CDC 串口并显示渐变灯效（cmd 0x1C）
    mlinkctl flash <file>       刷入固件：自动进更新模式，把 app 镜像推给设备，校验通过后设备自行复位

`mlinkctl --help` 与 `mlinkctl <command> --help` 由 argparse 生成。

不做循环、不做刷新，实时观察交给 shell：

    watch -n 0.2 -d mlinkctl
    while :; do mlinkctl; sleep 0.2; done

ping 是唯一的例外：设备端把探测交给协议层（Ping 控制帧发出、Pong 返回后刷新
平滑往返），命令响应里没有结果，因此它会轮询统计快照直到链路报告已连接或
等待超时。

速率请自行对两次快照的 *_bytes 差分（计数器为 u32 累计值，回绕按
(cur - prev) & 0xFFFFFFFF 处理）。

动作命令的 status=ok 只表示设备已接受投递：执行发生在设备主循环，
结果看设备日志与行为；reset / reboot / update 之后设备会重新枚举，注意等待。
更新模式（update）没有配置通道：那里的设备只暴露一条 CDC 串口（升级协议走它），
本工具对它的控制请求会得到传输错误，属预期；硬复位/断电即回正常模式。

Linux 非 root 访问需要在 /etc/udev/rules.d/99-meguminlink.rules 写一行：

    SUBSYSTEM=="usb", ATTR{idVendor}=="303a", ATTR{idProduct}=="4020", TAG+="uaccess"

协议见 ml-esp/main/config_channel.cc 顶部；本文件与固件一一对应。
"""

import argparse
import glob
import hashlib
import struct
import sys
import time
from pathlib import Path

import serial
import usb.core
import usb.util

VID = 0x303A
PID = 0x4020

VENDOR_CODE = 0x4D
SERIAL_HELP = "目标设备的序列号（多台在线时用它指定；默认要求只有一台）"
CMD_GET_STATS = 0x01
CMD_GET_CONFIG = 0x02
CMD_SET_LINK_CONFIG = 0x1B
CMD_GET_USB_CONFIG = 0x03
CMD_SET_MODE = 0x10
CMD_ENTER_PAIRING = 0x11
CMD_CLEAR_PAIR = 0x12
CMD_RESET = 0x13
CMD_REBOOT = 0x14
CMD_EXIT_PAIRING = 0x15
CMD_SET_USB_CONFIG = 0x16
CMD_APPLY_USB_CONFIG = 0x17
CMD_PING = 0x18
CMD_SET_WIFI_RATE = 0x19
CMD_SET_LOSS = 0x1A
CMD_ENTER_UPDATE = 0x1C
CMD_SET_UART_RELIABLE = 0x1D

# 更新模式的固件升级协议（见 ml-esp/main/usb/firmware_updater.cc 顶部；与固件一一对应）：
# 8B 帧头 + 载荷，CDC 串口上一问一答，整数一律大端。设备侧用 CDC 而不是 vendor
# 接口，主机因此按普通串口访问，不需要 udev 规则或 WinUSB 驱动。
UPDATE_PORT_MATCH = "MeguminLink_Update"
UPDATE_BAUD = 115200
UPDATE_MAGIC = 0x4D4F
UPDATE_HEADER = ">HBBHH"
UPDATE_HEADER_SIZE = 8
UPDATE_CHUNK_MAX = 4096

# Info 响应载荷布局：槽位大小 u32 | 分块上限 u32 | 版本串 32B | 设备 MAC 6B。
# MAC 是后加的尾部字段，老固件只回前 40 字节，那时无法校验目标身份。
UPDATE_INFO_SLOT_OFFSET = 0
UPDATE_INFO_VERSION_OFFSET = 8
UPDATE_INFO_VERSION_LEN = 32
UPDATE_INFO_MAC_OFFSET = UPDATE_INFO_VERSION_OFFSET + UPDATE_INFO_VERSION_LEN
UPDATE_INFO_MAC_LEN = 6

CMD_UPDATE_INFO = 0x01
CMD_UPDATE_BEGIN = 0x02
CMD_UPDATE_DATA = 0x03
CMD_UPDATE_FINISH = 0x04
CMD_UPDATE_ABORT = 0x05

UPDATE_STATUS = {
    0x00: "ok",
    0x02: "malformed",
    0x03: "internal",
    0x04: "unsupported",
    0x06: "no-partition",
    0x07: "hash-mismatch",
    0x08: "image-invalid",
    0x09: "size-exceeded",
    0x0A: "sequence",
    0x0B: "flash-error",
}

# 单帧响应超时（秒）：Begin 要擦除镜像覆盖的范围、Finish 要读回整个分区校验，都比数据帧慢
UPDATE_TIMEOUT_S = 10.0
# 数据帧只做一段 flash 写入：给一个较短的上限，异常时不必陪跑长时间
UPDATE_DATA_TIMEOUT_S = 3.0
# 等设备软复位后以更新模式重新枚举
UPDATE_ENTER_TIMEOUT_S = 15.0
# 刷完等设备复位回正常模式（引导 + 无线/USB 初始化）
UPDATE_RETURN_TIMEOUT_S = 20.0
# 进度输出步长（百分比）
UPDATE_PROGRESS_STEP = 5

MAGIC = 0x4D4C
FRAME_HEADER_SIZE = 8
MAX_RESPONSE = 512
TIMEOUT_MS = 1000
# ping 等待链路状态时的轮询间隔：一次控制帧往返远小于它，
# 因此不会明显推迟结果，也不会给 USB 控制端点造成压力
PING_POLL_S = 0.05

STATUS = {
    0x00: "ok",
    0x01: "unknown-command",
    0x02: "malformed",
    0x03: "internal",
    0x04: "unsupported",
    0x05: "busy",
}

# 统计快照输出字段与 tag 一一对应（顺序即打印顺序）
STATS_FIELDS = [
    (0x01, "mode"),
    (0x02, "pair"),
    (0x03, "rssi"),
    (0x10, "dap_cmds"),
    (0x11, "dap_req_bytes"),
    (0x12, "dap_resp_bytes"),
    (0x20, "wl_tx_bytes"),
    (0x21, "wl_rx_bytes"),
    (0x30, "uart_rx_bytes"),
    (0x31, "uart_tx_bytes"),
    (0x32, "uart_wl_rx_bytes"),
    (0x33, "uart_wl_tx_bytes"),
    (0x26, "loss_frames"),
    (0x27, "loss_dropped"),
    (0x71, "arq_tx"),
    (0x72, "arq_retx"),
    (0x74, "arq_failed"),
    (0x75, "arq_dup"),
    (0x7C, "arq_breaker_trips"),
    (0x7D, "arq_tx_dropped"),
    (0x7E, "arq_rx_dropped"),
    (0x40, "uptime_ms"),
]

# 配置快照输出字段与 tag 一一对应：0x41-0x43 是设备身份，0x50-0x53 是配置项
TAG_CONFIG_WIFI_RATE = 0x52
TAG_CONFIG_LOSS_PERMILLE = 0x53
CONFIG_FIELDS = [
    (0x41, "fw_version"),
    (0x42, "chip"),
    (0x43, "idf_version"),
    (0x50, "mode"),
    (0x51, "peer_mac"),
    (TAG_CONFIG_WIFI_RATE, "wifi_rate"),
    (TAG_CONFIG_LOSS_PERMILLE, "loss_percent"),
    (0x54, "arq_generation"),
    (0x56, "arq_window"),
    (0x57, "arq_max_retx"),
    (0x58, "arq_breaker"),
    (0x59, "arq_rto_min_ms"),
    (0x5B, "arq_rto_max_ms"),
    (0x5C, "arq_delayed_ack_ms"),
]

# CDC ACM 串口状态（GetStats 的 0x34-0x38，与固件 config_channel.cc 一致）：
# 打印顺序在计数器之后，老固件没有这些 tag 时整组跳过
TAG_UART_HOST_OPEN = 0x34
TAG_UART_BAUD = 0x35
TAG_UART_DATA_BITS = 0x36
TAG_UART_STOP_BITS = 0x37
TAG_UART_PARITY = 0x38
TAG_UART_RELIABLE = 0x39

# 对端存活探测（GetStats 的 0x22-0x23，与固件 config_channel.cc 一致）：
# online 1=已连接 2=未连接；rtt 是协议层平滑估计的微秒数
TAG_PEER_ONLINE = 0x22
TAG_PING_RTT = 0x23

PEER = {0: "unknown", 1: "online", 2: "offline"}

UART_STOP_BITS = {0: "1", 1: "1.5", 2: "2"}
UART_PARITY = {0: "N", 1: "O", 2: "E", 3: "M", 4: "S"}

MODE = {0: "local", 1: "wireless"}
PAIR = {0: "idle", 1: "paired", 2: "pairing"}
MODE_ARG = {"local": 0, "wireless": 1}

# ESP-NOW 发送速率档位：名字 → wifi_phy_rate_t 原值（与固件 wireless/link.cc
# 的 phymode_for_rate 认可的范围一致，即 CCK / OFDM / HT20 MCS0-7）。
# 名字按"调制族 + 标称速率"取，便于在实验里直接对照：档位越高越脆，长帧尤其明显。
WIFI_RATES = {
    "cck1-l": 0x00,   # 11b 1 Mbps 长前导
    "cck2-l": 0x01,
    "cck5-l": 0x02,
    "cck11-l": 0x03,
    "cck2-s": 0x05,
    "cck5-s": 0x06,
    "cck11-s": 0x07,
    "ofdm48": 0x08,
    "ofdm24": 0x09,
    "ofdm12": 0x0A,
    "ofdm6": 0x0B,
    "ofdm54": 0x0C,
    "ofdm36": 0x0D,
    "ofdm18": 0x0E,
    "ofdm9": 0x0F,
    "mcs0-lgi": 0x10,
    "mcs1-lgi": 0x11,
    "mcs2-lgi": 0x12,
    "mcs3-lgi": 0x13,
    "mcs4-lgi": 0x14,
    "mcs5-lgi": 0x15,
    "mcs6-lgi": 0x16,
    "mcs7-lgi": 0x17,
    "mcs0-sgi": 0x18,
    "mcs1-sgi": 0x19,
    "mcs2-sgi": 0x1A,
    "mcs3-sgi": 0x1B,
    "mcs4-sgi": 0x1C,
    "mcs5-sgi": 0x1D,
    "mcs6-sgi": 0x1E,
    "mcs7-sgi": 0x1F,
}
WIFI_RATE_NAMES = {value: name for name, value in WIFI_RATES.items()}
# 固件默认档（wireless/link.hpp 的 DEFAULT_WIFI_RATE）
WIFI_RATE_DEFAULT = 0x09

# 注入丢包率上限（百分比）：固件按千分比存放，wireless/link.hpp 的
# MAX_LOSS_PERMILLE = 255 即 25.5%。设备端只在发送端丢 DAP 与串口数据帧，
# 配置通道的控制帧永远放行，因此丢包率开满也改得回来。
LOSS_PERCENT_MAX = 25.5

# USB 能力（GetUsbConfig）的 tag 与取值域，与固件 usb_capability.hpp 一致
TAG_USB_SUPPORTED = 0x60
TAG_USB_WEBUSB = 0x61
TAG_USB_WINUSB = 0x62
TAG_USB_LANDING_PAGE = 0x63
TAG_USB_DRAFT = 0x64
TAG_USB_DRAFT_REASON = 0x65
TAG_USB_HID_COMPAT = 0x66
TAG_USB_DEVICE_NAME = 0x67

# landing page 上限 = esp_tinyusb 字符串描述符缓冲（128B）减头与结尾 NUL
USB_LANDING_PAGE_MAX_LEN = 126
# 设备名上限：拼上固定的 "(CMSIS-DAP)" 后正好 31 字符（esp_tinyusb 单字符串上限）
USB_DEVICE_NAME_MAX_LEN = 20
USB_CAPABILITY_WEBUSB = 0x01
USB_CAPABILITY_WINUSB = 0x02
USB_CAPABILITY_HID_COMPAT = 0x04
DRAFT = {0: "none", 1: "pending", 2: "rejected"}
REJECT = {1: "too-long", 2: "invalid-char", 3: "unsupported"}

# 无参数动作命令：子命令 → (命令码, 帮助文本)
ACTIONS = {
    "pair": (CMD_ENTER_PAIRING, "进入配对流程"),
    "exit-pair": (CMD_EXIT_PAIRING, "退出配对流程"),
    "unpair": (CMD_CLEAR_PAIR, "清除配对记录并重新配对"),
    "reset": (CMD_RESET, "恢复出厂设置：清除配对、模式与 USB 能力记录"),
    "reboot": (CMD_REBOOT, "重启设备"),
    "update": (CMD_ENTER_UPDATE, "进入更新模式：重启后只保留一个 CDC 串口，硬复位退出"),
}
# 执行后设备会重新枚举的子命令
REBOOTING = {"reset", "reboot", "update"}


def die(message):
    print(f"mlinkctl: {message}", file=sys.stderr)
    sys.exit(1)


def list_devices():
    """当前在线的 MeguminLink 设备（正常模式与更新模式共用同一 VID:PID）。"""
    return list(usb.core.find(find_all=True, idVendor=VID, idProduct=PID))


def device_serial(dev):
    """设备身份：USB iSerialNumber。固件两个模式报的都是芯片 factory MAC，前后一致。"""
    try:
        return str(dev.serial_number)
    except (usb.core.USBError, ValueError):
        return None  # 描述符还没读全：这台设备无法参与身份匹配


def find_device(serial=None):
    """挑出目标设备：serial 给定时必须在线，不给则要求只有一台。

    只回答"是哪台"，不回答"它现在处于哪个模式"——正常模式与更新模式的 VID:PID
    相同，模式判定交给调用方（见 enter_update_mode）。
    """
    devices = list_devices()
    if not devices:
        die(f"no MeguminLink device ({VID:04x}:{PID:04x}) found")

    online = ", ".join(filter(None, (device_serial(dev) for dev in devices)))
    if serial is not None:
        for dev in devices:
            if device_serial(dev) == serial:
                return dev
        die(f"没有序列号为 {serial} 的设备（在线：{online or '无'}）")

    if len(devices) > 1:
        die(f"发现 {len(devices)} 台设备（{online}），用 --serial 指定目标")
    return devices[0]


def target_serial(args):
    """本次操作的目标设备序列号：--serial 给了就用它，否则要求只有一台在线。"""
    mac = device_serial(find_device(args.serial))
    if mac is None:
        die("读不到设备序列号（USB 描述符未就绪）")
    return mac


def fetch(dev, cmd, arg=0):
    try:
        raw = dev.ctrl_transfer(0xC0, VENDOR_CODE, cmd | (arg << 8), 0, MAX_RESPONSE, TIMEOUT_MS)
    except usb.core.USBError as err:
        if err.errno in (32, 19):  # EPIPE/ENODEV: 设备对请求发 STALL
            die("firmware does not support the config channel (bRequest 0x4D)")
        raise
    return bytes(raw)


def decode(frame):
    if len(frame) < FRAME_HEADER_SIZE:
        die(f"short response ({len(frame)} bytes)")
    if int.from_bytes(frame[0:2], "big") != MAGIC:
        die("bad frame magic")
    status = frame[6]
    if status != 0x00:
        die(f"device returned status={STATUS.get(status, hex(status))}")

    values = {}
    pos = FRAME_HEADER_SIZE
    end = FRAME_HEADER_SIZE + int.from_bytes(frame[4:6], "big")
    while pos + 2 <= end:
        tag = frame[pos]
        length = frame[pos + 1]
        pos += 2
        if length == 0xFF:
            length = int.from_bytes(frame[pos : pos + 2], "big")
            pos += 2
        values[tag] = frame[pos : pos + length]
        pos += length
    return values


def render_stats(values):
    parts = []
    for tag, name in STATS_FIELDS:
        raw = values.get(tag)
        if raw is None:
            continue
        if name == "mode":
            value = MODE.get(int.from_bytes(raw, "big"), "?")
        elif name == "pair":
            value = PAIR.get(int.from_bytes(raw, "big"), "?")
        elif name == "rssi":
            value = int.from_bytes(raw, "big", signed=True)
        else:
            value = int.from_bytes(raw, "big")
        parts.append(f"{name}={value}")
    parts.extend(render_uart_state(values))
    parts.extend(render_peer_state(values))
    parts.extend(render_arq_state(values))
    return " ".join(parts)


def render_uart_state(values):
    """CDC ACM 串口状态：主机是否打开、线编码（老固件不上报时整组跳过）。"""
    if TAG_UART_HOST_OPEN not in values:
        return []

    def number(tag, default=0):
        raw = values.get(tag)
        return int.from_bytes(raw, "big") if raw else default

    data_bits = number(TAG_UART_DATA_BITS, 8)
    stop_bits = UART_STOP_BITS.get(number(TAG_UART_STOP_BITS), "?")
    parity = UART_PARITY.get(number(TAG_UART_PARITY), "?")
    parts = [
        f"uart_open={'yes' if number(TAG_UART_HOST_OPEN) else 'no'}",
        f"uart_baud={number(TAG_UART_BAUD)}",
        f"uart_format={data_bits}{parity}{stop_bits}",
    ]
    # 可靠出向是较新的 tag：老固件不上报时整项省略，别把缺失读成 off
    if TAG_UART_RELIABLE in values:
        parts.append(f"uart_reliable={'on' if number(TAG_UART_RELIABLE) else 'off'}")
    return parts


def render_peer_state(values):
    """对端存活探测：online 带协议层的平滑往返（老固件整组跳过）。"""
    if TAG_PEER_ONLINE not in values:
        return []

    online = u8_of(values, TAG_PEER_ONLINE)
    parts = [f"peer={PEER.get(online, '?')}"]
    if online == 1:
        parts.append(f"rtt_ms={u32_of(values, TAG_PING_RTT) / 1000:.2f}")
    return parts


# 可靠传输（GetStats 的 0x70-0x7F、GetConfig 的 0x54-0x5D，与固件 config_channel.cc 一致）
TAG_ARQ_STATE = 0x70
TAG_ARQ_ACKED = 0x73
TAG_ARQ_REPLAYED = 0x78
TAG_ARQ_REPLAY_MISS = 0x79
TAG_ARQ_SRTT_MS = 0x7A
TAG_ARQ_RTO_MS = 0x7B
TAG_ARQ_TX_DROPPED = 0x7D
TAG_ARQ_RX_DROPPED = 0x7E
TAG_ARQ_CMD_DROPPED = 0x7F
TAG_ARQ_GENERATION = 0x54
TAG_ARQ_RTO_INIT_MS = 0x5A
TAG_ARQ_PROBE_MS = 0x5D


def render_arq_state(values):
    """可靠传输状态：窗口/熔断位在 0x70 里打包，重传率由 tx 与 retx 现算（老固件整组跳过）。"""
    if TAG_ARQ_STATE not in values:
        return []

    state = u8_of(values, TAG_ARQ_STATE)
    tx = u32_of(values, 0x71)
    retx = u32_of(values, 0x72)
    rate = f"{retx / tx * 100:.1f}%" if tx else "—"
    parts = [
        f"arq_window={state & 0x0F}",
        f"arq_in_flight={(state >> 4) & 0x07}",
        f"arq_broken={'yes' if state & 0x80 else 'no'}",
        f"arq_srtt_ms={u32_of(values, TAG_ARQ_SRTT_MS)}",
        f"arq_rto_ms={u32_of(values, TAG_ARQ_RTO_MS)}",
        f"arq_retx_rate={rate}",
        f"arq_replayed={u32_of(values, TAG_ARQ_REPLAYED)}",
        f"arq_replay_miss={u32_of(values, TAG_ARQ_REPLAY_MISS)}",
    ]
    # 丢弃类计数：老固件不上报整组跳过；正常链路上这三个数应恒为 0
    if TAG_ARQ_TX_DROPPED in values:
        parts.extend(
            (
                f"arq_tx_dropped={u32_of(values, TAG_ARQ_TX_DROPPED)}",
                f"arq_rx_dropped={u32_of(values, TAG_ARQ_RX_DROPPED)}",
                f"arq_cmd_dropped={u32_of(values, TAG_ARQ_CMD_DROPPED)}",
            )
        )
    return parts


def format_mac(raw):
    """MAC 字节串 → "aa:bb:cc:dd:ee:ff"；空字节串返回 None。"""
    return ":".join(f"{byte:02x}" for byte in raw) if raw else None


def render_config(values):
    parts = []
    for tag, name in CONFIG_FIELDS:
        raw = values.get(tag)
        if name == "mode":
            value = MODE.get(int.from_bytes(raw, "big"), "?") if raw is not None else "?"
        elif name == "peer_mac":
            value = format_mac(raw) or "unpaired"
        elif name == "wifi_rate":
            # 未知档位（固件比本文件新）按原始值显示，不猜名字
            code = int.from_bytes(raw, "big") if raw else None
            value = (
                "?"
                if code is None
                else f"{WIFI_RATE_NAMES.get(code, 'unknown')}(0x{code:02x})"
            )
        elif name == "loss_percent":
            # 设备端按千分比存放，界面上统一按百分比呈现（0.1% 步进）
            code = int.from_bytes(raw, "big") if raw else None
            value = "?" if code is None else f"{code / 10:.1f}%"
        elif name.startswith("arq_"):
            # 可靠传输参数都是整数
            code = int.from_bytes(raw, "big") if raw else None
            value = "?" if code is None else code
        else:
            value = raw.decode("utf-8", "replace") if raw is not None else ""
        parts.append(f"{name}={value}")
    return " ".join(parts)


def u8_of(values, tag):
    raw = values.get(tag)
    return int.from_bytes(raw, "big") if raw else 0


def u16_of(values, tag):
    raw = values.get(tag)
    return int.from_bytes(raw, "big") if raw else 0


def u32_of(values, tag):
    raw = values.get(tag)
    return int.from_bytes(raw, "big") if raw else 0


def render_usb(values):
    supported = u8_of(values, TAG_USB_SUPPORTED)
    compiled = [
        name
        for bit, name in (
            (USB_CAPABILITY_WEBUSB, "webusb"),
            (USB_CAPABILITY_WINUSB, "winusb"),
            (USB_CAPABILITY_HID_COMPAT, "hid-compat"),
        )
        if supported & bit
    ]
    draft = u8_of(values, TAG_USB_DRAFT)
    parts = [
        f"compiled={','.join(compiled) if compiled else 'none'}",
        f"webusb={'on' if u8_of(values, TAG_USB_WEBUSB) else 'off'}",
        f"winusb={'on' if u8_of(values, TAG_USB_WINUSB) else 'off'}",
        f"hid_compat={'on' if u8_of(values, TAG_USB_HID_COMPAT) else 'off'}",
        f"landing_page={values.get(TAG_USB_LANDING_PAGE, b'').decode('utf-8', 'replace')}",
        f"name={values.get(TAG_USB_DEVICE_NAME, b'').decode('utf-8', 'replace')}",
        f"draft={DRAFT.get(draft, '?')}",
    ]
    if draft == 2:
        parts.append(f"draft_reason={REJECT.get(u8_of(values, TAG_USB_DRAFT_REASON), '?')}")
    return " ".join(parts)


def encode_tlv(tag, value):
    if len(value) >= 0xFF:
        return bytes([tag, 0xFF]) + len(value).to_bytes(2, "big") + value
    return bytes([tag, len(value)]) + value


def check_landing_page(url):
    if len(url) > USB_LANDING_PAGE_MAX_LEN:
        die(f"landing page 超过 {USB_LANDING_PAGE_MAX_LEN} 字符")
    for byte in url.encode("utf-8"):
        if byte < 0x20 or byte > 0x7E:
            die("landing page 只能是可打印 ASCII 字符")


def check_device_name(name):
    if len(name) > USB_DEVICE_NAME_MAX_LEN:
        die(f"设备名超过 {USB_DEVICE_NAME_MAX_LEN} 字符")
    for byte in name.encode("utf-8"):
        if byte < 0x20 or byte > 0x7E:
            die("设备名只能是可打印 ASCII 字符")


def cmd_read_stats(args):
    print(render_stats(decode(fetch(find_device(args.serial), CMD_GET_STATS))))


def cmd_read_config(args):
    print(render_config(decode(fetch(find_device(args.serial), CMD_GET_CONFIG))))


def cmd_read_usb(args):
    print(render_usb(decode(fetch(find_device(args.serial), CMD_GET_USB_CONFIG))))


def cmd_arq(args):
    """可靠传输的聚焦视图：状态、RTO 估计与全部计数各一行，便于盯链路质量。"""
    dev = find_device(args.serial)
    stats = decode(fetch(dev, CMD_GET_STATS))
    config = decode(fetch(dev, CMD_GET_CONFIG))

    if TAG_ARQ_STATE not in stats:
        die("固件不支持可靠传输（GetStats 里没有 0x70），或该功能未编译")

    state = u8_of(stats, TAG_ARQ_STATE)
    tx = u32_of(stats, 0x71)
    retx = u32_of(stats, 0x72)
    print(
        f"window={state & 0x0F}"
        f" in_flight={(state >> 4) & 0x07}"
        f" broken={'yes' if state & 0x80 else 'no'}"
        f" generation={u32_of(config, TAG_ARQ_GENERATION)}"
    )
    print(
        f"rto_ms={u32_of(stats, TAG_ARQ_RTO_MS)}"
        f" srtt_ms={u32_of(stats, TAG_ARQ_SRTT_MS)}"
        f" min={u16_of(config, 0x59)}"
        f" init={u16_of(config, TAG_ARQ_RTO_INIT_MS)}"
        f" max={u16_of(config, 0x5B)}"
    )
    print(
        f"tx={tx} retx={retx}"
        f" retx_rate={f'{retx / tx * 100:.1f}%' if tx else '—'}"
        f" failed={u32_of(stats, 0x74)}"
        f" breaker_trips={u32_of(stats, 0x7C)}"
    )
    print(
        f"acked={u32_of(stats, TAG_ARQ_ACKED)}"
        f" dup={u32_of(stats, 0x75)}"
        f" ooo={u32_of(stats, 0x76)}"
        f" skipped={u32_of(stats, 0x77)}"
        f" replayed={u32_of(stats, TAG_ARQ_REPLAYED)}"
        f" replay_miss={u32_of(stats, TAG_ARQ_REPLAY_MISS)}"
    )
    if TAG_ARQ_TX_DROPPED in stats:
        print(
            f"tx_dropped={u32_of(stats, TAG_ARQ_TX_DROPPED)}"
            f" rx_dropped={u32_of(stats, TAG_ARQ_RX_DROPPED)}"
            f" cmd_dropped={u32_of(stats, TAG_ARQ_CMD_DROPPED)}"
        )


def cmd_set_link(args):
    """下发可靠传输参数：只覆盖指定的字段，代数由设备自己 +1。"""
    u8_items = [
        ("window", 0x02, args.window),
        ("max-retx", 0x03, args.max_retx),
        ("breaker", 0x04, args.breaker),
    ]
    u16_items = [
        ("rto-min", 0x05, args.rto_min),
        ("rto-init", 0x06, args.rto_init),
        ("rto-max", 0x07, args.rto_max),
        ("delayed-ack", 0x08, args.delayed_ack),
        ("probe", 0x09, args.probe),
    ]

    payload = b""
    for name, tag, value in u8_items:
        if value is None:
            continue
        if not 0 <= value <= 0xFF:
            die(f"--{name} 超出 u8 范围：{value}")
        payload += encode_tlv(tag, bytes([value]))
    for name, tag, value in u16_items:
        if value is None:
            continue
        if not 1 <= value <= 0xFFFF:
            die(f"--{name} 超出 u16 范围：{value}")
        payload += encode_tlv(tag, value.to_bytes(2, "big"))

    if not payload:
        die("至少要指定一个参数（--window / --rto-min / --max-retx / …）")

    dev = find_device(args.serial)
    generation = u32_of(decode(fetch(dev, CMD_GET_CONFIG)), TAG_ARQ_GENERATION)

    try:
        dev.ctrl_transfer(0x40, VENDOR_CODE, CMD_SET_LINK_CONFIG, 0, payload, TIMEOUT_MS)
    except usb.core.USBError as err:
        die(f"设备拒绝了该载荷（参数越界或 TLV 格式非法）：{err}")

    # 参数由主循环应用并落盘，代数随之 +1。OUT 请求没有响应阶段，只能盯着代数等它
    # 变——不等的话读到的还是旧值（应用还没跑到）。
    deadline = time.time() + 2.0
    config = decode(fetch(dev, CMD_GET_CONFIG))
    while u32_of(config, TAG_ARQ_GENERATION) == generation and time.time() < deadline:
        time.sleep(0.05)
        config = decode(fetch(dev, CMD_GET_CONFIG))

    if u32_of(config, TAG_ARQ_GENERATION) == generation:
        die("设备没有应用这份参数（代数未变化），检查设备日志")

    print(render_config(config))
    print(
        "mlinkctl: 参数随即生效并重置会话，对端由会话重置帧同步；设备已自行落盘",
        file=sys.stderr,
    )


def cmd_set_usb(args):
    if (
        args.webusb is None
        and args.winusb is None
        and args.hid_compat is None
        and args.landing_page is None
        and args.name is None
    ):
        die("至少要指定 --webusb / --winusb / --hid-compat / --landing-page / --name 之一")

    dev = find_device(args.serial)
    current = decode(fetch(dev, CMD_GET_USB_CONFIG))

    webusb = u8_of(current, TAG_USB_WEBUSB)
    winusb = u8_of(current, TAG_USB_WINUSB)
    hid_compat = u8_of(current, TAG_USB_HID_COMPAT)
    landing = current.get(TAG_USB_LANDING_PAGE, b"").decode("utf-8", "replace")
    name = current.get(TAG_USB_DEVICE_NAME, b"").decode("utf-8", "replace")

    if args.webusb is not None:
        webusb = 1 if args.webusb == "on" else 0
    if args.winusb is not None:
        winusb = 1 if args.winusb == "on" else 0
    if args.hid_compat is not None:
        hid_compat = 1 if args.hid_compat == "on" else 0
    if args.landing_page is not None:
        landing = args.landing_page
    if args.name is not None:
        name = args.name
    check_landing_page(landing)
    check_device_name(name)

    # 全量提交三项；HID 兼容模式与设备名只在指定时下发，设备端沿用当前值
    payload = (
        encode_tlv(TAG_USB_WEBUSB, bytes([webusb]))
        + encode_tlv(TAG_USB_WINUSB, bytes([winusb]))
        + encode_tlv(TAG_USB_LANDING_PAGE, landing.encode("utf-8"))
    )
    if args.hid_compat is not None:
        payload += encode_tlv(TAG_USB_HID_COMPAT, bytes([hid_compat]))
    if args.name is not None:
        payload += encode_tlv(TAG_USB_DEVICE_NAME, name.encode("utf-8"))
    try:
        dev.ctrl_transfer(0x40, VENDOR_CODE, CMD_SET_USB_CONFIG, 0, payload, TIMEOUT_MS)
    except usb.core.USBError as err:
        die(f"设备拒绝了该载荷（格式非法、能力未编译或传输失败）：{err}")

    # OUT 请求没有响应阶段：草稿状态由随后的一次 GetUsbConfig 报出
    status = decode(fetch(dev, CMD_GET_USB_CONFIG))
    state = u8_of(status, TAG_USB_DRAFT)
    if state == 2:
        die(f"设备拒绝了该配置：{REJECT.get(u8_of(status, TAG_USB_DRAFT_REASON), '?')}")
    if state != 1:
        die("设备端没有待应用的草稿（可能被其他客户端清掉了）")

    decode(fetch(dev, CMD_APPLY_USB_CONFIG))
    print(
        f"status=ok webusb={'on' if webusb else 'off'} winusb={'on' if winusb else 'off'} "
        f"hid_compat={'on' if hid_compat else 'off'} landing_page={landing} name={name}"
    )
    print("mlinkctl: 设备正在重新枚举 USB，稍后需要重新识别设备", file=sys.stderr)


def cmd_set_mode(args):
    decode(fetch(find_device(args.serial), CMD_SET_MODE, MODE_ARG[args.mode]))
    print("status=ok")


def cmd_set_rate(args):
    if args.list:
        for name, value in sorted(WIFI_RATES.items(), key=lambda item: item[1]):
            mark = " (默认)" if value == WIFI_RATE_DEFAULT else ""
            print(f"0x{value:02x}  {name}{mark}")
        return

    if args.rate is None:
        die("需要给出速率档位（--list 可列出全部）")
    if args.rate in WIFI_RATES:
        value = WIFI_RATES[args.rate]
    else:
        try:
            value = int(args.rate, 0)
        except ValueError:
            die(f"未知档位 {args.rate!r}；--list 可列出全部")
        if value not in WIFI_RATE_NAMES:
            die(f"档位 0x{value:02x} 不在固件支持范围内（--list 可列出全部）")

    # 设备端应用到本地单播 peer，并把同一条控制请求转发给对端：两端跑同一档位，
    # 上行与下行的丢包率才可比。转发是尽力而为的，对端没跟上时重发一次即可。
    decode(fetch(find_device(args.serial), CMD_SET_WIFI_RATE, value))
    print(f"status=ok rate={WIFI_RATE_NAMES.get(value, 'unknown')}(0x{value:02x})")
    print("mlinkctl: 档位已下发本机并转发对端（对端未跟随时重发一次）", file=sys.stderr)


def cmd_set_loss(args):
    try:
        percent = float(args.percent)
    except ValueError:
        die(f"丢包率需要是百分比数值，收到 {args.percent!r}")

    # 区间与一位小数一起校验：NaN 与 inf 都过不了这个连比
    if not 0.0 <= percent <= LOSS_PERCENT_MAX:
        die(f"丢包率需要在 0-{LOSS_PERCENT_MAX}% 之间（0.1% 步进），收到 {args.percent!r}")

    scaled = percent * 10
    permille = int(round(scaled))
    if abs(scaled - permille) > 1e-6:
        die(f"丢包率最多一位小数（0.1% 步进），收到 {args.percent!r}")

    # 设备端按千分比存放，并把同一条控制请求转发给对端：注入只在发送端生效，
    # 两端必须各自持有同一个值，否则只有单方向的帧会被丢。转发是尽力而为的，
    # 对端没跟上时重发一次即可。
    decode(fetch(find_device(args.serial), CMD_SET_LOSS, permille))
    print(f"status=ok loss={percent:.1f}%")
    print("mlinkctl: 已下发本机并转发对端；两端固件都需编译该功能（见 Kconfig 的 Wireless 菜单）", file=sys.stderr)


def cmd_set_uart_reliable(args):
    value = 1 if args.state == "on" else 0

    # 设备端按 0/1 存放，并把同一条控制请求转发给对端：两端必须一致，只改一端会
    # 变成"下载走可靠、回包走尽力而为"的半可靠链路，现象没法归因。转发是尽力而
    # 为的，对端没跟上时重发一次即可。
    decode(fetch(find_device(args.serial), CMD_SET_UART_RELIABLE, value))
    print(f"status=ok uart_reliable={args.state}")
    if value == 0:
        print("mlinkctl: 已关闭：串口出向回到尽力而为，超速的帧会被令牌桶整帧丢弃", file=sys.stderr)


def cmd_action(args):
    decode(fetch(find_device(args.serial), args.cmd_code))
    print("status=ok")
    if args.command in REBOOTING:
        print("mlinkctl: 设备正在重启，稍后需重新枚举", file=sys.stderr)


def find_update_port(mac, timeout_s):
    """等这台设备的更新模式 CDC 串口出现。

    端口名要同时带更新模式产品名与这台设备的序列号才算数：同机插多台时不会拿错，
    别的设备处于更新模式也不会被当成目标。返回 None 表示超时（timeout_s 为 0 时只查一次）。
    """
    deadline = time.monotonic() + timeout_s
    while True:
        for path in sorted(glob.glob("/dev/serial/by-id/*")):
            if UPDATE_PORT_MATCH in path and mac in path:
                return path
        if time.monotonic() >= deadline:
            return None
        time.sleep(0.2)


def enter_update_mode(mac):
    """返回该设备更新模式 CDC 串口的设备路径：已在更新模式就直接用，否则软复位后等它出现。

    进更新模式这一步只能走配置通道（正常模式 vendor 接口上的 EP0 厂商请求），仍然
    需要那个设备节点的访问权限；升级数据本身走 CDC，不再需要。
    """
    port = find_update_port(mac, 0.0)
    if port is not None:
        return port

    decode(fetch(find_device(mac), CMD_ENTER_UPDATE))
    print("mlinkctl: 设备正在软复位进更新模式", file=sys.stderr)
    port = find_update_port(mac, UPDATE_ENTER_TIMEOUT_S)
    if port is None:
        die(f"等待 {UPDATE_ENTER_TIMEOUT_S:.0f}s 未见 {mac} 的更新模式 CDC 串口")
    return port


def recv_exact(port, size, timeout_s):
    """按字节数收满：CDC 是字节流，响应帧边界由帧头声明的长度决定。"""
    port.timeout = timeout_s
    data = port.read(size)
    if len(data) != size:
        die(f"串口读超时：期望 {size} 字节，收到 {len(data)}")
    return data


def recv_update_frame(port, timeout_s):
    header = recv_exact(port, UPDATE_HEADER_SIZE, timeout_s)
    magic, frame_type, status, length, seq = struct.unpack(UPDATE_HEADER, header)
    if magic != UPDATE_MAGIC:
        die(f"响应帧魔数错误：0x{magic:04x}")
    payload = recv_exact(port, length, timeout_s) if length else b""
    return frame_type, status, seq, payload


def update_request(port, frame_type, seq, payload=b"", timeout_s=UPDATE_TIMEOUT_S):
    """发一条命令并等它对应的响应：响应错位或状态非 ok 都在这里终止。"""
    header = struct.pack(UPDATE_HEADER, UPDATE_MAGIC, frame_type, 0x00, len(payload), seq)
    port.write(header + payload)

    resp_type, status, resp_seq, resp_payload = recv_update_frame(port, timeout_s)
    if resp_type != frame_type or resp_seq != seq:
        die(f"响应错位：命令 0x{frame_type:02x}/seq {seq} 收到 0x{resp_type:02x}/seq {resp_seq}")
    if status != 0x00:
        die(f"设备返回 status={UPDATE_STATUS.get(status, hex(status))}")
    return resp_payload


def wait_for_running_device(mac, timeout_s):
    """等这台设备复位回正常模式并能应答配置通道：读到配置快照才算真的回来了。"""
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        dev = next((candidate for candidate in list_devices() if device_serial(candidate) == mac), None)
        if dev is not None:
            try:
                raw = dev.ctrl_transfer(0xC0, VENDOR_CODE, CMD_GET_CONFIG, 0, MAX_RESPONSE, TIMEOUT_MS)
                return decode(bytes(raw))
            except usb.core.USBError:
                pass  # 还在初始化：配置通道没起来，继续等
        time.sleep(0.3)
    return None


def cmd_flash(args):
    """把 app 镜像刷进设备：进更新模式 → Info 预检（含目标身份核对）→ Begin → Data 分块 → Finish。"""
    path = Path(args.firmware)
    if not path.is_file():
        die(f"{args.firmware} 不是文件")
    image = path.read_bytes()
    if not image:
        die(f"{args.firmware} 是空文件")
    digest = hashlib.sha256(image).digest()

    mac = target_serial(args)
    port_path = enter_update_mode(mac)
    port = serial.Serial(port_path, UPDATE_BAUD, timeout=UPDATE_TIMEOUT_S)
    try:
        info = update_request(port, CMD_UPDATE_INFO, 1, timeout_s=args.timeout / 1000)
        partition_size, max_chunk = struct.unpack(">II", info[UPDATE_INFO_SLOT_OFFSET:UPDATE_INFO_VERSION_OFFSET])
        version = (
            info[UPDATE_INFO_VERSION_OFFSET:UPDATE_INFO_MAC_OFFSET].split(b"\x00", 1)[0].decode("utf-8", "replace")
        )
        if partition_size == 0:
            die("设备没有可写的 OTA 槽（分区表里没有第二个 app 分区）")
        if len(image) > partition_size:
            die(f"镜像 {len(image)} 字节超过槽位 {partition_size} 字节")

        # 串口名里的 MAC 只是路径约定，这里再让设备自报一次身份：认错了就一个字节都不写
        device_mac = info[UPDATE_INFO_MAC_OFFSET:UPDATE_INFO_MAC_OFFSET + UPDATE_INFO_MAC_LEN]
        if len(device_mac) == UPDATE_INFO_MAC_LEN and any(device_mac) and format_mac(device_mac) != mac:
            die(f"串口 {port_path} 上的设备是 {format_mac(device_mac)}，不是目标 {mac}")

        chunk = min(args.chunk or max_chunk, max_chunk, UPDATE_CHUNK_MAX)
        if chunk <= 0:
            die(f"分块大小非法：{chunk}")
        print(
            f"mlinkctl: 目标 {mac}，设备当前固件 {version}，经 {port_path} 写入 {len(image)} 字节（分块 {chunk}）",
            file=sys.stderr,
        )

        seq = 2
        update_request(
            port, CMD_UPDATE_BEGIN, seq, struct.pack(">I", len(image)) + digest, timeout_s=args.timeout / 1000
        )

        sent = 0
        next_mark = UPDATE_PROGRESS_STEP
        started = time.monotonic()
        while sent < len(image):
            block = image[sent : sent + chunk]
            seq += 1
            update_request(port, CMD_UPDATE_DATA, seq, block, timeout_s=UPDATE_DATA_TIMEOUT_S)
            sent += len(block)

            percent = sent * 100 // len(image)
            if percent >= next_mark or sent == len(image):
                elapsed = time.monotonic() - started
                rate = sent / elapsed / 1024 if elapsed > 0 else 0.0
                print(
                    f"\r  {percent:3d}%  {sent / 1024:7.1f} KiB  {rate:6.1f} KiB/s",
                    end="",
                    file=sys.stderr,
                    flush=True,
                )
                next_mark = percent + UPDATE_PROGRESS_STEP

        seq += 1
        update_request(port, CMD_UPDATE_FINISH, seq, timeout_s=args.timeout / 1000)
        elapsed = time.monotonic() - started
        print(file=sys.stderr)
    finally:
        port.close()

    print(
        f"status=ok bytes={len(image)} sha256={digest.hex()} "
        f"elapsed_s={elapsed:.1f} rate_kb_s={len(image) / elapsed / 1024:.1f}"
    )
    print("mlinkctl: 镜像已校验、启动分区已切换，设备正在复位进新固件", file=sys.stderr)

    values = wait_for_running_device(mac, UPDATE_RETURN_TIMEOUT_S)
    if values is None:
        print(f"mlinkctl: 等待 {UPDATE_RETURN_TIMEOUT_S:.0f}s 未见设备回到正常模式，请查设备日志", file=sys.stderr)
        return
    print(
        f"mlinkctl: 设备已回到正常模式，fw_version={values.get(0x41, b'').decode('utf-8', 'replace')}",
        file=sys.stderr,
    )


def probe_once(dev, timeout_ms):
    """发一次对端探测，返回 (online, rtt_us)。

    设备端把命令交给协议层：Ping 控制帧发出、Pong 返回后刷新平滑往返时延，命令
    响应里没有单次结果。这里在超时窗口内轮询统计快照：协议连接保持即认为对端
    在线，往返取协议层的平滑估计（微秒）。
    """
    before = decode(fetch(dev, CMD_GET_STATS))
    if TAG_PEER_ONLINE not in before:
        die("固件不支持对端探测（GetStats 里没有 peer_online），请更新固件")

    decode(fetch(dev, CMD_PING))

    deadline = time.monotonic() + timeout_ms / 1000
    while time.monotonic() < deadline:
        time.sleep(PING_POLL_S)
        stats = decode(fetch(dev, CMD_GET_STATS))
        if u8_of(stats, TAG_PEER_ONLINE) == 1:
            return True, u32_of(stats, TAG_PING_RTT)

    return False, 0


def cmd_ping(args):
    if args.count < 1:
        die("探测次数至少为 1")

    dev = find_device(args.serial)
    rtt_ms = []
    for index in range(args.count):
        if index > 0:
            time.sleep(args.interval / 1000)
        online, rtt_us = probe_once(dev, args.timeout)
        if online:
            rtt_ms.append(rtt_us / 1000)
            print(f"peer=online rtt_ms={rtt_ms[-1]:.2f}")
        else:
            print("peer=offline rtt_ms=-")

    if args.count > 1:
        lost = args.count - len(rtt_ms)
        if rtt_ms:
            print(
                f"--- {args.count} 次探测：ok={len(rtt_ms)} fail={lost} "
                f"loss={lost * 100 / args.count:.0f}% rtt_min_ms={min(rtt_ms):.2f} "
                f"rtt_avg_ms={sum(rtt_ms) / len(rtt_ms):.2f} rtt_max_ms={max(rtt_ms):.2f}"
            )
        else:
            print(f"--- {args.count} 次探测：ok=0 fail={lost} loss=100%")


def build_parser():
    parser = argparse.ArgumentParser(
        prog="mlinkctl",
        description="MeguminLink 配置通道探针：读统计/配置，或投递控制动作。",
        epilog="协议见 ml-esp/main/config_channel.cc 顶部；本文件与固件一一对应。",
    )
    parser.add_argument("--serial", metavar="MAC", help=SERIAL_HELP)
    # 无子命令 = 读一次统计快照（历史行为，也是日常用得最多的那件事）
    parser.set_defaults(handler=cmd_read_stats)

    subparsers = parser.add_subparsers(dest="command", metavar="<command>")
    subparsers.add_parser("config", help="读设备信息与配置项").set_defaults(handler=cmd_read_config)
    subparsers.add_parser(
        "usb", help="读 USB 能力（WebUSB / WinUSB / HID 兼容模式 / landing page / 设备名称）"
    ).set_defaults(handler=cmd_read_usb)

    set_usb = subparsers.add_parser("set-usb", help="设置 USB 能力与设备名称，保存后设备自动重新枚举")
    set_usb.add_argument("--webusb", choices=("on", "off"), help="WebUSB 能力开关")
    set_usb.add_argument("--winusb", choices=("on", "off"), help="WinUSB 自动识别驱动开关")
    set_usb.add_argument(
        "--hid-compat",
        choices=("on", "off"),
        help="HID 兼容模式：暴露 CMSIS-DAP v1 的 HID 接口（未编译该功能时设备拒绝开启）",
    )
    set_usb.add_argument("--landing-page", metavar="URL", help="WebUSB landing page URL（空串回退默认值）")
    set_usb.add_argument(
        "--name",
        metavar="NAME",
        help=f"设备名称：可打印 ASCII、最多 {USB_DEVICE_NAME_MAX_LEN} 字符（空串清除，产品名回到默认）",
    )
    set_usb.set_defaults(handler=cmd_set_usb)

    set_mode = subparsers.add_parser("set-mode", help="切换运行模式")
    set_mode.add_argument("mode", choices=sorted(MODE_ARG), help="local 或 wireless")
    set_mode.set_defaults(handler=cmd_set_mode)

    set_link = subparsers.add_parser("set-link", help="设置可靠传输参数（随即生效并同步给对端）")
    set_link.add_argument("--window", type=int, metavar="N", help="发送窗口（本固件上限 8，默认 4）")
    set_link.add_argument("--rto-min", type=int, metavar="MS", help="RTO 下限（毫秒）")
    set_link.add_argument("--rto-init", type=int, metavar="MS", help="还没有 RTT 样本时的初始 RTO（毫秒）")
    set_link.add_argument("--rto-max", type=int, metavar="MS", help="RTO 上限（毫秒，退避到此为止）")
    set_link.add_argument("--max-retx", type=int, metavar="N", help="单条消息重传次数上限")
    set_link.add_argument("--breaker", type=int, metavar="N", help="连续多少条重传耗尽后熔断（0 = 不熔断）")
    set_link.add_argument("--delayed-ack", type=int, metavar="MS", help="没有数据可捎带时补纯 ACK 的延迟")
    set_link.add_argument("--probe", type=int, metavar="MS", help="熔断期间放行试探的间隔")
    set_link.set_defaults(handler=cmd_set_link)

    subparsers.add_parser("arq", help="可靠传输聚焦视图：状态、RTO 估计与全部计数").set_defaults(
        handler=cmd_arq
    )

    set_rate = subparsers.add_parser("set-rate", help="设置 ESP-NOW 发送速率档位（两端同时应用）")
    set_rate.add_argument("rate", nargs="?", help="档位名（如 mcs7-sgi）或数值（如 0x1F）")
    set_rate.add_argument("--list", action="store_true", help="列出全部档位后退出")
    set_rate.set_defaults(handler=cmd_set_rate)

    set_loss = subparsers.add_parser("set-loss", help="设置注入丢包率，两端同时应用（模拟信号变差）")
    set_loss.add_argument(
        "percent",
        metavar="PERCENT",
        help=f"丢包率百分比，0-{LOSS_PERCENT_MAX}（0.1 步进），0 = 关闭",
    )
    set_loss.set_defaults(handler=cmd_set_loss)

    set_uart = subparsers.add_parser("set-uart-reliable", help="串口出向是否走可靠传输，两端同时应用")
    set_uart.add_argument(
        "state",
        choices=("on", "off"),
        help="on = 可靠传输（丢帧重传，占一个 ARQ 窗口）；off = 尽力而为（超速即丢）",
    )
    set_uart.set_defaults(handler=cmd_set_uart_reliable)

    ping = subparsers.add_parser("ping", help="探测已配对对端（ESP-NOW 往返时延）")
    ping.add_argument("-c", "--count", type=int, default=1, metavar="N", help="探测次数（默认 1）")
    ping.add_argument("-i", "--interval", type=int, default=500, metavar="MS", help="两次探测之间的间隔毫秒（默认 500）")
    ping.add_argument(
        "-w",
        "--timeout",
        type=int,
        default=1000,
        metavar="MS",
        help="单次探测等待链路报告在线的毫秒（默认 1000）",
    )
    ping.set_defaults(handler=cmd_ping)

    flash = subparsers.add_parser("flash", help="刷入固件：自动进更新模式，校验通过后设备自行复位")
    flash.add_argument("firmware", metavar="FILE", help="app 镜像，如 build-s3-pv2/megumin-link-esp.bin")
    flash.add_argument("--chunk", type=int, metavar="N", help="单帧数据字节数（默认用设备上报的上限）")
    flash.add_argument(
        "--timeout",
        type=int,
        default=int(UPDATE_TIMEOUT_S * 1000),
        metavar="MS",
        help=f"单帧响应超时毫秒（默认 {int(UPDATE_TIMEOUT_S * 1000)}，Begin 擦除与 Finish 校验较慢）",
    )
    flash.set_defaults(handler=cmd_flash)

    for name, (code, help_text) in ACTIONS.items():
        subparsers.add_parser(name, help=help_text).set_defaults(handler=cmd_action, cmd_code=code)

    # 目标选择写在子命令后面也认：mlinkctl flash fw.bin --serial <MAC>
    for sub in subparsers.choices.values():
        sub.add_argument("--serial", metavar="MAC", help=SERIAL_HELP)

    return parser


def main(argv=None):
    args = build_parser().parse_args(argv)
    args.handler(args)


if __name__ == "__main__":
    main()
