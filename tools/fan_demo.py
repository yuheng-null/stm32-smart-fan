#!/usr/bin/env python3
"""跑一次「阶跃辨识 + 闭环跟踪」的演示，并生成 README 里那两张图。

用法::

    python tools/fan_demo.py                 # COM7，输出到 docs/img/
    python tools/fan_demo.py --port COM3     # 换串口
    python tools/fan_demo.py --no-step       # 只测闭环

它做的事（全程自动，不需要人看着）::

    1. STREAM 1                切到 CSV 遥测
    2. MODE MANUAL / MANUAL 20 稳定到 20% 占空比
    3. MANUAL 60               打一个 +40% 的阶跃 → 辨识时间常数 τ
    4. MODE AUTO / TEMP 10     切回闭环，曲线左移使目标落在可达区
    5. 出图 + 打印 τ

为什么要把它做成脚本而不是手工跑：**τ 是 PI 整定的地基**
（见 docs/control.md §4 里那次「按假定 τ 整定导致上板振荡」的事故）。
换风扇、改风道之后必须重测，那重测就得是一件事先写好的、可重复的操作，
而不是「照着文档手工敲几条命令」。

⚠️ 跑之前确认风扇的 12V 已接好、且是**先让板子跑起来再给的风扇电**
（反过来 PWM 引脚浮空 = 风扇内部上拉 = 全速）。
"""

from __future__ import annotations

import argparse
import csv
import math
import os
import re
import sys
import time
from pathlib import Path

import serial

# Windows 控制台默认 cp1252/cp936，直接 print 中文会抛 UnicodeEncodeError。
for _stream in (sys.stdout, sys.stderr):
    if hasattr(_stream, "reconfigure"):
        _stream.reconfigure(encoding="utf-8", errors="replace")

# 与 firmware 的 telemetry_format(TELEMETRY_FMT_CSV) 逐字段对应
FRAME = re.compile(
    rb"@TEL,(\d+),(\d+),(\d+),(\d+),(-?\d+),(\d+),(\d+),(\d+),(\d+)"
)

# 经验模型（README 实测拟合）：rpm ≈ K × duty% + B。用来算阶跃的稳态值。
K_RPM_PER_PERCENT = 163.0
RPM_INTERCEPT = 190.0

STEP_LOW_PERCENT = 20
STEP_HIGH_PERCENT = 60


def send(port: serial.Serial, text: str) -> None:
    port.write(text.encode("ascii") + b"\r\n")
    port.flush()


def collect(port: serial.Serial, seconds: float) -> list[tuple[float, int, int, int, int]]:
    """收 seconds 秒的 @TEL 帧，返回 (相对时刻, 设备 tick, duty, rpm, 目标 rpm)。"""
    out: list[tuple[float, int, int, int, int]] = []
    buf = b""
    started = time.monotonic()

    while time.monotonic() - started < seconds:
        buf += port.read(4096)

        while b"\n" in buf:
            line, _, buf = buf.partition(b"\n")
            m = FRAME.match(line.strip())
            if m:
                f = [int(x) for x in m.groups()]
                out.append((time.monotonic() - started, f[1], f[3], f[6], f[7]))

    return out


def find_tau(step: list[tuple[float, int, int, int, int]]) -> float | None:
    """按 63% 规则求时间常数。

    用**63% 规则**而不是对 ln 做线性拟合：这条曲线的起步有一段平坦
    （静摩擦 + 风扇内部限速），拟合会被那一段带偏，实测下来拟合值
    只有 0.6 s 左右，而 63% 规则给 1.4 s —— 后者才与闭环整定的结果是自洽的。
    """
    low = K_RPM_PER_PERCENT * STEP_LOW_PERCENT + RPM_INTERCEPT
    high = K_RPM_PER_PERCENT * STEP_HIGH_PERCENT + RPM_INTERCEPT
    threshold = low + 0.63 * (high - low)

    for t, _tick, _duty, rpm, _target in step:
        if rpm >= threshold:
            return t

    return None


def plot_step(step, tau, out_path: Path) -> None:
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    low = K_RPM_PER_PERCENT * STEP_LOW_PERCENT + RPM_INTERCEPT
    high = K_RPM_PER_PERCENT * STEP_HIGH_PERCENT + RPM_INTERCEPT

    fig, ax = plt.subplots(figsize=(8, 4.2))
    ax.plot([s[0] for s in step], [s[3] for s in step], color="tab:red", lw=1.6,
            label="measured rpm")

    for level in (low, high):
        ax.axhline(level, color="gray", lw=0.8, ls=":")
    ax.axhline(low + 0.63 * (high - low), color="green", lw=0.8, ls=":")

    if tau is not None:
        ax.axvline(tau, color="green", lw=1.0)
        ax.annotate(
            f"63% of the step  ->  tau = {tau:.2f} s",
            xy=(tau, low + 0.63 * (high - low) + 400),
            xytext=(tau + 1.0, low + 0.63 * (high - low) + 1500),
            color="green", arrowprops=dict(arrowstyle="->", color="green"),
        )

    ax.annotate(f"{STEP_LOW_PERCENT}% duty\n{low:.0f} rpm", xy=(0.2, low),
                xytext=(6, low - 700), color="gray", fontsize=8, ha="center")
    ax.annotate(f"{STEP_HIGH_PERCENT}% duty\n{high:.0f} rpm", xy=(16, high),
                xytext=(14, high - 900), color="gray", fontsize=8, ha="center")

    ax.set_xlabel("time since step (s)")
    ax.set_ylabel("fan speed (rpm)")
    ax.set_title(
        f"Plant identification: step {STEP_LOW_PERCENT}% -> {STEP_HIGH_PERCENT}% duty (MANUAL mode)"
    )
    ax.grid(alpha=0.3)
    ax.legend(loc="center right", fontsize=8)
    fig.tight_layout()
    fig.savefig(out_path, dpi=130)
    plt.close(fig)


def plot_closed_loop(loop, out_path: Path) -> None:
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    fig, (a1, a2) = plt.subplots(2, 1, figsize=(8, 4.8), sharex=True)

    a1.plot([s[0] for s in loop], [s[3] for s in loop], color="tab:red", lw=1.5,
            label="measured rpm")
    a1.plot([s[0] for s in loop], [s[4] for s in loop], color="black", lw=1.1,
            ls="--", label="target rpm")
    a1.set_ylabel("rpm")
    a1.grid(alpha=0.3)
    a1.legend(loc="lower right", fontsize=8)
    a1.set_title("Closed loop: measured tracks target (switch MANUAL -> AUTO)")

    a2.plot([s[0] for s in loop], [s[2] for s in loop], color="tab:green", lw=1.5)
    a2.set_ylabel("duty (%)")
    a2.set_xlabel("time since switch (s)")
    a2.grid(alpha=0.3)

    fig.tight_layout()
    fig.savefig(out_path, dpi=130)
    plt.close(fig)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", default="COM7")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--out-dir", default=None,
                        help="图片输出目录（默认 <仓库>/docs/img）")
    parser.add_argument("--csv", default=os.path.join(
        os.environ.get("TEMP", "."), "smart_fan_demo.csv"),
        help="把原始帧也存一份，便于事后分析")
    parser.add_argument("--no-step", action="store_true",
                        help="跳过阶跃辨识，只测闭环")
    args = parser.parse_args()

    out_dir = Path(args.out_dir) if args.out_dir else \
        Path(__file__).resolve().parent.parent / "docs" / "img"
    out_dir.mkdir(parents=True, exist_ok=True)

    step: list = []
    loop: list = []

    with serial.Serial(args.port, args.baud, timeout=0.1) as port:
        port.reset_input_buffer()
        send(port, "STREAM 1")

        if not args.no_step:
            send(port, "MODE MANUAL")
            send(port, f"MANUAL {STEP_LOW_PERCENT}")
            print(f"稳定到 {STEP_LOW_PERCENT}% ...")
            collect(port, 14)

            port.reset_input_buffer()
            send(port, f"MANUAL {STEP_HIGH_PERCENT}")
            print(f"阶跃到 {STEP_HIGH_PERCENT}% ...")
            step = collect(port, 16)

        send(port, "MODE AUTO")
        send(port, "TEMP 10")          # 曲线左移，让目标落在可达区间内
        print("闭环 ...")
        loop = collect(port, 24)

    with open(args.csv, "w", newline="", encoding="utf-8") as fh:
        writer = csv.writer(fh)
        writer.writerow(["phase", "t_s", "tick_ms", "duty", "rpm", "target_rpm"])
        for phase, rows in (("step", step), ("loop", loop)):
            for row in rows:
                writer.writerow([phase, *row])

    if step:
        tau = find_tau(step)
        plot_step(step, tau, out_dir / "step-response.png")
        print(f"tau = {tau:.2f} s" if tau else "阶跃没到 63%，τ 未测出（幅值太小？）")
        print(f"  -> {out_dir / 'step-response.png'}")

    if loop:
        plot_closed_loop(loop, out_dir / "closed-loop.png")
        steady = [s[3] for s in loop[-30:]]
        target = loop[-1][4]
        if target:
            err = max(abs(max(steady) - target), abs(min(steady) - target)) / target * 100
            print(f"稳态: 目标 {target} / 实测 {min(steady)}~{max(steady)} -> 最大偏差 {err:.1f}%")
        print(f"  -> {out_dir / 'closed-loop.png'}")

    print(f"原始帧: {args.csv}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
