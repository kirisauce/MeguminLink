#!/usr/bin/env python3
"""mlinkctl —— MeguminLink 配置通道的命令行探针。

一次调用 = 一次 EP0 厂商控制请求（bRequest 0x4D），打印一行 key=value 后退出：

    mlinkctl                     读一次统计快照（cmd 0x01 GET_STATS）
    mlinkctl config              读设备信息与配置项（cmd 0x02 GET_CONFIG）
    mlinkctl set-mode <mode>     切换运行模式：local | wireless（cmd 0x10）
    mlinkctl pair                进入配对流程（cmd 0x11）
    mlinkctl unpair              清除配对记录并重新配对（cmd 0x12）
    mlinkctl reset               清除配对与模式记录后重启（cmd 0x13）
    mlinkctl reboot              重启设备（cmd 0x14）

`mlinkctl --help` 与 `mlinkctl <command> --help` 由 argparse 生成。

不做循环、不做刷新，实时观察交给 shell：

    watch -n 0.2 -d mlinkctl
    while :; do mlinkctl; sleep 0.2; done

速率请自行对两次快照的 *_bytes 差分（计数器为 u32 累计值，回绕按
(cur - prev) & 0xFFFFFFFF 处理）。

动作命令的 status=ok 只表示设备已接受投递：执行发生在设备主循环，
结果看设备日志与行为；reset 与 reboot 之后设备会重新枚举，注意等待。

Linux 非 root 访问需要在 /etc/udev/rules.d/99-meguminlink.rules 写一行：

    SUBSYSTEM=="usb", ATTR{idVendor}=="303a", ATTR{idProduct}=="4020", TAG+="uaccess"

协议见 ml-esp/main/config_channel.cc 顶部；本文件与固件一一对应。
"""

import argparse
import sys

import usb.core

VID = 0x303A
PID = 0x4020

VENDOR_CODE = 0x4D
CMD_GET_STATS = 0x01
CMD_GET_CONFIG = 0x02
CMD_SET_MODE = 0x10
CMD_ENTER_PAIRING = 0x11
CMD_CLEAR_PAIR = 0x12
CMD_RESET = 0x13
CMD_REBOOT = 0x14

MAGIC = 0x4D4C
FRAME_HEADER_SIZE = 8
MAX_RESPONSE = 512
TIMEOUT_MS = 1000

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
    (0x40, "uptime_ms"),
]

# 配置快照输出字段与 tag 一一对应：0x41-0x43 是设备身份，0x50/0x51 是配置项
CONFIG_FIELDS = [
    (0x41, "fw_version"),
    (0x42, "chip"),
    (0x43, "idf_version"),
    (0x50, "mode"),
    (0x51, "peer_mac"),
]

MODE = {0: "local", 1: "wireless"}
PAIR = {0: "idle", 1: "paired", 2: "pairing"}
MODE_ARG = {"local": 0, "wireless": 1}

# 无参数动作命令：子命令 → (命令码, 帮助文本)
ACTIONS = {
    "pair": (CMD_ENTER_PAIRING, "进入配对流程"),
    "unpair": (CMD_CLEAR_PAIR, "清除配对记录并重新配对"),
    "reset": (CMD_RESET, "清除配对与模式记录后重启"),
    "reboot": (CMD_REBOOT, "重启设备"),
}
# 执行后设备会重新枚举的子命令
REBOOTING = {"reset", "reboot"}


def die(message):
    print(f"mlinkctl: {message}", file=sys.stderr)
    sys.exit(1)


def find_device():
    devices = list(usb.core.find(find_all=True, idVendor=VID, idProduct=PID))
    if not devices:
        die(f"no MeguminLink device ({VID:04x}:{PID:04x}) found")
    if len(devices) > 1:
        serials = ", ".join(str(dev.serial_number) for dev in devices)
        die(f"{len(devices)} MeguminLink devices found ({serials}), connect only one")
    return devices[0]


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
    return " ".join(parts)


def render_config(values):
    parts = []
    for tag, name in CONFIG_FIELDS:
        raw = values.get(tag)
        if name == "mode":
            value = MODE.get(int.from_bytes(raw, "big"), "?") if raw is not None else "?"
        elif name == "peer_mac":
            value = ":".join(f"{byte:02x}" for byte in raw) if raw else "unpaired"
        else:
            value = raw.decode("utf-8", "replace") if raw is not None else ""
        parts.append(f"{name}={value}")
    return " ".join(parts)


def cmd_read_stats(_args):
    print(render_stats(decode(fetch(find_device(), CMD_GET_STATS))))


def cmd_read_config(_args):
    print(render_config(decode(fetch(find_device(), CMD_GET_CONFIG))))


def cmd_set_mode(args):
    decode(fetch(find_device(), CMD_SET_MODE, MODE_ARG[args.mode]))
    print("status=ok")


def cmd_action(args):
    decode(fetch(find_device(), args.cmd_code))
    print("status=ok")
    if args.command in REBOOTING:
        print("mlinkctl: 设备正在重启，稍后需重新枚举", file=sys.stderr)


def build_parser():
    parser = argparse.ArgumentParser(
        prog="mlinkctl",
        description="MeguminLink 配置通道探针：读统计/配置，或投递控制动作。",
        epilog="协议见 ml-esp/main/config_channel.cc 顶部；本文件与固件一一对应。",
    )
    # 无子命令 = 读一次统计快照（历史行为，也是日常用得最多的那件事）
    parser.set_defaults(handler=cmd_read_stats)

    subparsers = parser.add_subparsers(dest="command", metavar="<command>")
    subparsers.add_parser("config", help="读设备信息与配置项").set_defaults(handler=cmd_read_config)

    set_mode = subparsers.add_parser("set-mode", help="切换运行模式")
    set_mode.add_argument("mode", choices=sorted(MODE_ARG), help="local 或 wireless")
    set_mode.set_defaults(handler=cmd_set_mode)

    for name, (code, help_text) in ACTIONS.items():
        subparsers.add_parser(name, help=help_text).set_defaults(handler=cmd_action, cmd_code=code)

    return parser


def main(argv=None):
    args = build_parser().parse_args(argv)
    args.handler(args)


if __name__ == "__main__":
    main()
