#!/usr/bin/env python3
"""MeguminLink 探针带宽压测：持续对目标内存做批量写入与回读校验，逐轮统计稳态吞吐。

跑法（仓库里没有 pyproject，pyocd 由 uv tool 提供，所以用 uv 的临时环境取模块）：
    uv run --with pyocd python tools/benchmark.py
    uv run --with pyocd python tools/benchmark.py --seconds 60
    uv run --with pyocd python tools/benchmark.py --mode write --size 262144
    ~/.local/share/uv/tools/pyocd/bin/python tools/benchmark.py   # 直接用装了 pyocd 的解释器

口径：
  - 写 = Target.write_memory_block8 一次提交一整块；读 = read_memory_block8，并与写入
    内容逐字节比对。吞吐按 1 MB = 1e6 字节算，统计只含 DAP 传输与 pyOCD 的 API 开销，
    不含任何落盘。
  - 预热一轮不计入统计（避开首次 DAP 事务与链路稳定过程）。
  - 无线与本地的差别只在设备的 mode（mlinkctl set-mode local|wireless），脚本不区分：
    两者都经 USB 上那台 S3 的 CMSIS-DAP 接口。

注意：压测会覆盖 --addr 起 --size 字节的目标内存，并在结束时恢复目标运行；目标上正在
跑的程序会被打断。轮次跑满（--rounds）或时间到点（--seconds）就汇总，Ctrl-C 也会先打印
已经跑出来的统计。--rounds 0 --seconds 0 表示一直跑到 Ctrl-C。
"""

import argparse
import random
import statistics
import sys
import time

MB = 1_000_000
SRAM_SEED = 0x4D4547


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="MeguminLink 探针带宽压测（pyOCD 口径）")
    parser.add_argument("--target", default="stm32f407vetx", help="pyOCD target 名（默认 stm32f407vetx）")
    parser.add_argument("--addr", type=lambda v: int(v, 0), default=0x20000000, help="起始地址（默认 0x20000000）")
    parser.add_argument("--size", type=int, default=98304, help="每轮字节数（默认 98304 = 96 KiB）")
    parser.add_argument("--rounds", type=int, default=20, help="轮次上限，0 = 不限（默认 20）")
    parser.add_argument("--seconds", type=float, default=0.0, help="持续秒数，0 = 只按轮次（默认 0）")
    parser.add_argument("--mode", choices=("both", "read", "write"), default="both", help="测哪一侧（默认 both）")
    parser.add_argument("--frequency", type=int, help="SWD/JTAG 时钟 kHz（默认用探针默认值）")
    parser.add_argument("--probe", help="pyOCD 探针 unique id（同时插着多个探针时必须给）")
    return parser.parse_args()


def sample_bytes(size: int) -> bytes:
    """确定性伪随机内容：各轮之间一致，回读比对能抓出位翻转"""
    return random.Random(SRAM_SEED).randbytes(size)


def mbps(size: int, seconds: float) -> float:
    return size / seconds / MB if seconds > 0 else 0.0


def format_side(name: str, samples: list[float]) -> str:
    return (
        f"{name}_MBps avg={statistics.fmean(samples):.3f} "
        f"min={min(samples):.3f} max={max(samples):.3f}"
    )


def main() -> int:
    args = parse_args()
    if args.size <= 0 or args.rounds < 0 or args.seconds < 0:
        print("尺寸必须为正，轮次与时长不能为负", file=sys.stderr)
        return 2

    try:
        from pyocd.core.helpers import ConnectHelper
    except ImportError:
        print(
            "找不到 pyocd：用 `uv run --with pyocd python tools/benchmark.py ...`，"
            "或换成已装 pyocd 的解释器（如 ~/.local/share/uv/tools/pyocd/bin/python）",
            file=sys.stderr,
        )
        return 1

    data = sample_bytes(args.size)
    write_side = args.mode in ("both", "write")
    read_side = args.mode in ("both", "read")

    try:
        session = ConnectHelper.session_with_chosen_probe(
            target_override=args.target,
            frequency=args.frequency,
            unique_id=args.probe,
        )
    except Exception as err:  # 探针掉线或被占用时 pyOCD 会抛带内部断言的异常，不值得甩栈
        print(f"连接探针失败：{type(err).__name__}: {err}", file=sys.stderr)
        print("检查探针是否在线、CMSIS-DAP 接口是否被 WebUI(WebUSB) 或别的工具占用", file=sys.stderr)
        return 1

    if session is None:
        print("pyocd: 找不到可用探针", file=sys.stderr)
        return 1

    write_samples: list[float] = []
    read_samples: list[float] = []
    mismatched = 0
    rounds = 0

    try:
        with session:
            target = session.target
            target.reset_and_halt()

            # read 模式没有可比对的内容：先写一份基准
            if not write_side:
                target.write_memory_block8(args.addr, data)

            # 预热一轮，不计入统计
            if write_side:
                target.write_memory_block8(args.addr, data)
            if read_side:
                bytes(target.read_memory_block8(args.addr, args.size))

            started = time.perf_counter()
            while True:
                if args.rounds > 0 and rounds >= args.rounds:
                    break
                if args.seconds > 0 and (time.perf_counter() - started) >= args.seconds:
                    break
                rounds += 1

                line = f"round={rounds}"
                matched = True
                if write_side:
                    t0 = time.perf_counter()
                    target.write_memory_block8(args.addr, data)
                    elapsed = time.perf_counter() - t0
                    write_samples.append(mbps(args.size, elapsed))
                    line += f" write_MBps={write_samples[-1]:.3f}"
                if read_side:
                    t0 = time.perf_counter()
                    readback = bytes(target.read_memory_block8(args.addr, args.size))
                    elapsed = time.perf_counter() - t0
                    read_samples.append(mbps(args.size, elapsed))
                    matched = readback == data
                    mismatched += 0 if matched else 1
                    line += f" read_MBps={read_samples[-1]:.3f} match={'yes' if matched else 'NO'}"
                print(line, flush=True)

            # 压测覆盖了目标内存，把目标放回运行态，别留一个停住的目标
            target.resume()
    except KeyboardInterrupt:
        print("已中断，下面是已跑出的统计", file=sys.stderr)
    except Exception as err:  # 链路中途断掉时也别丢已经跑出来的数
        print(f"压测中断：{type(err).__name__}: {err}", file=sys.stderr)

    if rounds == 0:
        print("一轮都没跑完", file=sys.stderr)
        return 1

    summary = f"BENCH rounds={rounds} size={args.size}"
    if write_samples:
        summary += " " + format_side("write", write_samples)
    if read_samples:
        summary += " " + format_side("read", read_samples)
        summary += f" match_all={'yes' if mismatched == 0 else 'NO'}"
    print(summary)
    return 0 if mismatched == 0 else 2


if __name__ == "__main__":
    sys.exit(main())
