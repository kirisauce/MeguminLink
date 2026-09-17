#!/usr/bin/env python3
"""mlinkctl —— 读一次 MeguminLink 设备的统计快照，打印一行 key=value 后退出。

一次调用 = 一次 EP0 厂商控制请求（bRequest 0x4D, cmd 0x01 GET_STATS）。
不做循环、不做刷新，实时观察交给 shell：

    watch -n 0.2 -d mlinkctl
    while :; do mlinkctl; sleep 0.2; done

速率请自行对两次快照的 *_bytes 差分（计数器为 u32 累计值，回绕按
(cur - prev) & 0xFFFFFFFF 处理）。

Linux 非 root 访问需要在 /etc/udev/rules.d/99-meguminlink.rules 写一行：

    SUBSYSTEM=="usb", ATTR{idVendor}=="303a", ATTR{idProduct}=="4020", TAG+="uaccess"

协议见 ml-esp/main/config_channel.cc 顶部；本文件与固件一一对应。
"""

import sys

import usb.core

VID = 0x303A
PID = 0x4020

VENDOR_CODE = 0x4D
CMD_GET_STATS = 0x01
MAGIC = 0x4D4C
FRAME_HEADER_SIZE = 8
MAX_RESPONSE = 512
TIMEOUT_MS = 1000

STATUS = {0x00: "ok", 0x01: "unknown-command", 0x02: "malformed", 0x03: "internal", 0x04: "unsupported"}

# 输出字段与 tag 一一对应（顺序即打印顺序）
FIELDS = [
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
MODE = {0: "local", 1: "wireless"}
PAIR = {0: "idle", 1: "paired", 2: "pairing"}


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


def fetch(dev):
    try:
        raw = dev.ctrl_transfer(0xC0, VENDOR_CODE, CMD_GET_STATS, 0, MAX_RESPONSE, TIMEOUT_MS)
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


def render(values):
    parts = []
    for tag, name in FIELDS:
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


def main():
    dev = find_device()
    print(render(decode(fetch(dev))))


if __name__ == "__main__":
    main()
