#!/usr/bin/env python3
"""把固件发出来的 `@TEL` 遥测帧抽成 CSV，并（可选）画成图。

用法::

    # 1) 先抓一段（用仓库里现成的抓取脚本，切到 CSV 模式）
    python tools/capture_serial.py --echo="STREAM 1" --seconds 30

    # 2) 从抓到的原始字节里抽帧
    python tools/plot_telemetry.py %TEMP%/smart_fan_serial_capture.txt

    # 也可以直接读串口
    python tools/plot_telemetry.py --port COM7 --seconds 30 --stream

产出：
    <输入>.csv   —— 表头与 firmware 的 telemetry_csv_header() 一致，可直接喂 pandas
    <输入>.png   —— 若装了 matplotlib 就顺手画一张四联图（温控调试主要看它）

为什么要单独写这个：调 PI 靠的是「目标转速 vs 实测转速 vs 占空比」三条线叠在
一起看。盯着串口滚动的十六进制数调参，是在浪费生命。
"""

from __future__ import annotations

import argparse
import csv
import re
import sys
from pathlib import Path

# Windows 控制台默认 cp1252/cp936，直接 print 中文会抛 UnicodeEncodeError。
for _stream in (sys.stdout, sys.stderr):
    if hasattr(_stream, "reconfigure"):
        _stream.reconfigure(encoding="utf-8", errors="replace")

# 与 firmware 的 telemetry_format(TELEMETRY_FMT_CSV) 逐字段对应。
# 改固件那份格式时，这里和 README 的表格要一起改。
FIELDS = [
    "seq",
    "t_ms",
    "mode",
    "duty",
    "temp_mc",
    "adc",
    "rpm",
    "target_rpm",
    "alarms",
]

FRAME_RE = re.compile(rb"^@TEL,(-?\d+),(-?\d+),(\d+),(\d+),(-?\d+),(\d+),(\d+),(\d+),(\d+)\s*$")

MODE_NAMES = {
    0: "INIT",
    1: "OFF",
    2: "MANUAL",
    3: "AUTO",
    4: "DEGR",
    5: "FAULT",
}


def parse_frames(raw: bytes) -> list[dict[str, int]]:
    """从原始字节流里抽出所有 @TEL 帧。

    按字节正则匹配、不先做整体解码：串口流里混着人类可读行（可能含多字节
    UTF-8）时，整体解码会在半个汉字上出错，把后面的帧一起吃掉。
    """
    rows: list[dict[str, int]] = []

    for line in raw.splitlines():
        m = FRAME_RE.match(line.strip())
        if not m:
            continue

        values = [int(g) for g in m.groups()]
        rows.append(dict(zip(FIELDS, values)))

    return rows


def report(rows: list[dict[str, int]]) -> None:
    if not rows:
        print("没找到任何 @TEL 帧。")
        print("（确认固件在 CSV 模式：发 STREAM 1；人类可读行不带 @TEL 前缀。）")
        return

    seqs = [r["seq"] for r in rows]
    gaps = sum(1 for a, b in zip(seqs, seqs[1:]) if ((b - a) & 0xFFFF) != 1)

    print(f"帧数        {len(rows)}")
    print(f"时长        {(rows[-1]['t_ms'] - rows[0]['t_ms']) / 1000.0:.2f} s")
    print(f"丢帧        {gaps} 处（seq 不连续）")

    temps = [r["temp_mc"] for r in rows if r["temp_mc"] != 0 or r["rpm"] != 0]
    if temps:
        print(f"温度范围    {min(temps)/1000.0:.1f} ~ {max(temps)/1000.0:.1f} ℃")

    rpms = [r["rpm"] for r in rows]
    print(f"转速范围    {min(rpms)} ~ {max(rpms)}")

    modes = sorted({r["mode"] for r in rows})
    print(f"出现的模式  {', '.join(MODE_NAMES.get(m, str(m)) for m in modes)}")

    alarm_values = sorted({r["alarms"] for r in rows})
    if alarm_values != [0]:
        print(f"报警位图    {[hex(a) for a in alarm_values]}")


def write_csv(rows: list[dict[str, int]], path: Path) -> None:
    with path.open("w", newline="", encoding="utf-8") as fh:
        writer = csv.DictWriter(fh, fieldnames=FIELDS)
        writer.writeheader()
        writer.writerows(rows)
    print(f"CSV         {path}")


def try_plot(rows: list[dict[str, int]], path: Path) -> None:
    """画一张四联图。没装 matplotlib 就跳过——不把绘图变成硬依赖。"""
    try:
        import matplotlib

        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("（未安装 matplotlib，跳过绘图。pip install matplotlib 可启用。）")
        return

    t = [(r["t_ms"] - rows[0]["t_ms"]) / 1000.0 for r in rows]

    fig, axes = plt.subplots(4, 1, sharex=True, figsize=(10, 8))

    axes[0].plot(t, [r["temp_mc"] / 1000.0 for r in rows], color="tab:red")
    axes[0].set_ylabel("温度 (℃)")

    axes[1].plot(t, [r["rpm"] for r in rows], label="实测")
    axes[1].plot(t, [r["target_rpm"] for r in rows], label="目标", linestyle="--")
    axes[1].set_ylabel("转速 (观测单位)")
    axes[1].legend()

    axes[2].plot(t, [r["duty"] for r in rows], color="tab:green")
    axes[2].set_ylabel("占空比 (%)")

    axes[3].plot(t, [r["adc"] for r in rows], color="tab:gray")
    axes[3].set_ylabel("ADC 原始计数")
    axes[3].set_xlabel("时间 (s)")

    for ax in axes:
        ax.grid(True, alpha=0.3)

    fig.suptitle("smart_fan 遥测")
    fig.tight_layout()
    fig.savefig(path, dpi=120)
    print(f"图          {path}")


def read_serial(port: str, baud: int, seconds: float, stream: bool) -> bytes:
    import time

    import serial  # pyserial

    with serial.Serial(port, baud, timeout=0.1) as ser:
        if stream:
            payload = b"STREAM 1\r\n"
            ser.write(payload)
            ser.flush()
            print(f"已发送 {payload!r} 切到 CSV 模式")

        raw = bytearray()
        started = time.monotonic()

        while time.monotonic() - started < seconds:
            chunk = ser.read(4096)
            if chunk:
                raw.extend(chunk)

    return bytes(raw)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("capture", nargs="?", help="capture_serial.py 抓下来的原始文件")
    parser.add_argument("--port", default=None, help="改为直接从串口读")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--seconds", type=float, default=30.0)
    parser.add_argument("--stream", action="store_true",
                        help="先发 STREAM 1 切到 CSV 模式（仅 --port 时有效）")
    parser.add_argument("--csv", default=None, help="CSV 输出路径（默认与输入同名）")
    parser.add_argument("--no-plot", action="store_true")
    args = parser.parse_args()

    if args.port:
        raw = read_serial(args.port, args.baud, args.seconds, args.stream)
        base = Path(f"telemetry_{args.port}")
    elif args.capture:
        raw = Path(args.capture).read_bytes()
        base = Path(args.capture)
    else:
        parser.error("要么给一个抓包文件，要么用 --port")
        return 2

    rows = parse_frames(raw)
    report(rows)

    if not rows:
        return 1

    csv_path = Path(args.csv) if args.csv else base.with_suffix(".csv")
    write_csv(rows, csv_path)

    if not args.no_plot:
        try_plot(rows, base.with_suffix(".png"))

    return 0


if __name__ == "__main__":
    sys.exit(main())
