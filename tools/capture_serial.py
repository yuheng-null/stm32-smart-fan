#!/usr/bin/env python3
"""抓取 CH340 串口输出，用来验证固件是否真的在按预期发送。

用法:
    python tools/capture_serial.py [--port COM7] [--baud 115200] [--seconds 6]

做法：**先打开串口，再在保持打开的前提下复位 MCU**。
顺序反过来的话，只发一次的开机横幅会在串口打开之前就发完，
抓到的只剩心跳，还得再跑一遍。

读的是**原始字节**，不是解码后的字符串——UTF-8 的中文按 ASCII 解码会全变成
'?'，看起来像固件乱码，实际是抓取侧的问题。

每条完整行的到达时刻都会打上墙钟时间戳。这是对时钟树的独立验证：
遥测是按 HAL_GetTick() 排的（200 ms 一拍），而 tick 由 HCLK 分频得到；
若实际晶振与 HSE_VALUE 不符，真实间隔就会按比例偏离 200 ms。

`--echo` 发命令之后，判据是**有没有收到 `@ACK`**，而不是「字节被原样回显」——
固件有意不回显（回显会和遥测流叠在一起变噪音），确认由 `@ACK,<verb>,OK` 承担。
命令没到达 MCU 就不会有任何回执，所以这条判据同样能证明 RX 通路是通的。

如果串口助手已经占着这个口，会报 PermissionError —— 先关掉它。
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import time
from pathlib import Path

import serial

# Windows 控制台默认 cp1252/cp936，直接 print 中文会抛 UnicodeEncodeError。
for _stream in (sys.stdout, sys.stderr):
    if hasattr(_stream, "reconfigure"):
        _stream.reconfigure(encoding="utf-8", errors="replace")

DEFAULT_PORT = "COM7"
DEFAULT_BAUD = 115200
DEFAULT_SECONDS = 6.0
# 收满一行就记时间戳；没换行的碎数据先攒着
READ_CHUNK = 4096

# 固件的人类可读遥测行长这样：[  1452 ms] AUTO   T 26.0C  adc 2000  duty   0% ...
TELEMETRY_RE = re.compile(r"^\[\s*\d+ ms\]")


def find_programmer_cli() -> Path | None:
    """在本机 ST 的 bundles 目录里找烧录 CLI（版本号现查，不硬记）。"""
    base = Path(os.environ.get("LOCALAPPDATA", "")) / "stm32cube" / "bundles" / "programmer"
    if not base.is_dir():
        return None
    candidates = sorted(base.iterdir(), reverse=True)
    for version_dir in candidates:
        cli = version_dir / "bin" / "STM32_Programmer_CLI.exe"
        if cli.is_file():
            return cli
    return None


def reset_target(cli: Path) -> None:
    """复位 MCU，让开机横幅落进我们正在监听的缓冲里。"""
    subprocess.Popen(
        [str(cli), "-c", "port=SWD", "mode=UR", "-rst"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default=DEFAULT_PORT)
    parser.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    parser.add_argument("--seconds", type=float, default=DEFAULT_SECONDS)
    parser.add_argument(
        "--echo",
        default=None,
        help="先把这个字符串发给板子，再看它回什么——用来验证 RX（PA10）。"
        "给了这个参数就不复位，以免开机横幅混进回显里。",
    )
    parser.add_argument(
        "--out",
        default=str(Path(os.environ.get("TEMP", ".")) / "smart_fan_serial_capture.txt"),
    )
    args = parser.parse_args()

    try:
        port = serial.Serial(args.port, args.baud, timeout=0.1)
    except serial.SerialException as exc:
        print(f"FAILED_TO_OPEN {args.port}: {exc}", file=sys.stderr)
        print("（串口助手是否还占着这个口？先关掉它再跑。）", file=sys.stderr)
        return 1

    print(f"PORT_OPEN {args.port} @ {args.baud}")

    if args.echo is None:
        cli = find_programmer_cli()
        if cli is not None:
            reset_target(cli)
            print(f"RESET via {cli.name}")
        else:
            print("WARN: 没找到 STM32_Programmer_CLI，跳过复位——将只能看到心跳，看不到横幅。")
    else:
        # 把打开端口之前积压的数据丢掉，免得和回显混在一起
        port.reset_input_buffer()

    raw = bytearray()
    pending = bytearray()
    stamped: list[tuple[float, str]] = []
    started = time.monotonic()

    with port:
        if args.echo is not None:
            payload = args.echo.encode("ascii")
            port.write(payload)
            port.flush()
            print(f"TX {len(payload)} bytes: {args.echo!r}")

        while True:
            elapsed = time.monotonic() - started
            if elapsed >= args.seconds:
                break

            chunk = port.read(READ_CHUNK)
            if not chunk:
                continue
            raw.extend(chunk)
            pending.extend(chunk)

            while b"\n" in pending:
                line, _, rest = pending.partition(b"\n")
                pending = bytearray(rest)
                text = line.decode("utf-8", errors="replace").rstrip("\r")
                if text:
                    stamped.append((time.monotonic() - started, text))

    out_path = Path(args.out)
    out_path.write_bytes(bytes(raw))

    print(f"\nCAPTURED_BYTES {len(raw)}")
    print(f"CAPTURE_FILE   {out_path}\n")
    print("---- 按到达时刻解出的行 ----")
    for at, text in stamped:
        print(f"[{at:6.3f}s] {text}")

    # 遥测行间隔是对时钟的独立验证，单独算一遍。
    # 固件按 200 ms 排遥测（APP_TASK_SLOW_MS）；间隔由 HAL_GetTick() 决定，
    # 而 tick 由 HCLK 分频而来——若实际晶振与 HSE_VALUE 不符，真实间隔会按比例偏移。
    beats = [at for at, text in stamped if TELEMETRY_RE.match(text)]
    if len(beats) >= 3:
        gaps = [b - a for a, b in zip(beats, beats[1:])]
        gaps = [g for g in gaps if g < 1.0]          # 丢掉被人为打断的那些
        if gaps:
            avg = sum(gaps) / len(gaps)
            print("\n---- 遥测间隔 ----")
            print("  " + "  ".join(f"{g:.3f}s" for g in gaps[:12]))
            print(f"  平均 {avg:.3f}s（固件按 200 ms 排；明显偏离说明实际主频与配置不符）")
            print(f"  → 实测主频约 {168_000_000 * (0.200 / avg) / 1e6:.1f} MHz"
                  if avg > 0 else "")

    # 命令响应判定：发出去的命令有没有得到回执。
    #
    # 固件**有意不回显**收到的字节——回显会和 200 ms 一帧的遥测叠在一起变成噪音，
    # 确认改由 `@ACK,<verb>,OK` / `@ACK,<verb>,ERR,<code>` 承担。
    # 所以这里判据是「有没有收到 @ACK」，它同样能证明 RX（PA10）通路是通的：
    # 命令没到达 MCU，就不会有任何回执。
    if args.echo is not None:
        payload = args.echo.encode("ascii")
        print("\n---- 命令响应 ----")
        print(f"  发出 {len(payload)} 字节: {payload!r}")
        print(f"  RX 共收 {len(raw)} 字节")

        acks = [line for line in raw.decode("utf-8", "replace").splitlines()
                if line.startswith("@ACK,")]

        for line in acks:
            print(f"  {line}")

        if acks:
            print("  OK: 收到回执 —— PA10(RX) 通路正常，命令已到达 MCU")
        else:
            print("  FAIL: 没有任何 @ACK 回执 —— 检查 PA10 接线与共地，")
            print("        以及命令行是否以 \\r 或 \\n 结束（行协议必须有结束符）")
            return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
