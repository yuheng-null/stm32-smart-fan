#!/usr/bin/env python3
"""把 cube-recipes/ 下的配方下达给工程的 .ioc（驱动 STM32CubeMX 脚本模式）。

用法::

    python tools/cubemx_apply.py apply      <工程根> <配方文件>
    python tools/cubemx_apply.py roundtrip  <工程根>
    python tools/cubemx_apply.py regenerate <工程根>

为什么需要这个脚本
------------------

`.ioc` 是 CubeMX **内部状态的序列化**，不是可以直接手写的配置文件。
手写 `Mcu.IPn=TIM1` 这类「该外设已激活」的**记录**，CubeMX 重建不出对应状态，
会把整块**静默丢弃**——不报错、不警告，参数连同记录一起没了。

所以改配置只能经 CubeMX 自己的脚本模式：`STM32CubeMX.exe -q <脚本文件>`。

`apply` 做的事
--------------

1. `config load <工程.ioc>`
2. 逐行执行配方里的命令
3. `config saveas` 到临时文件
4. 把原 `.ioc` 备份成 `<名>.ioc.bak`，再用临时文件覆盖它

**为什么不能直接在原文件上改**：`saveas` 是 CubeMX 唯一的落盘方式。

`roundtrip` 做的事
------------------

让 CubeMX 加载一遍再存出来，然后比对两边的键——报告哪些被丢弃、哪些被改写。
它是「参数被接受了吗」的判据。

注意它**证明不了「取值合法」**：CubeMX 对不认识的取值照样原样存回。
唯一会明说这件事的是 CubeMX 的日志
（`~/.stm32cubemx/STM32CubeMX.log`，每次运行覆盖写）。本脚本会在跑完后
把日志里的 `OptionalMessage` 行打出来——**那才是「取值合法」的判据**。

`regenerate` 做的事
-------------------

`apply` **只改 `.ioc`，不生成代码**。要出代码必须再跑一次 CubeMX 的生成，
本命令就是那一步。

生成会**重写** `Core/` 下的文件，CubeMX 靠「读回 `USER CODE` 块」保留用户代码，
而这个机制**偶尔会静默失效**（块被整段清空、返回码仍为 0、一句报错都没有）。
所以本命令：

1. 先把 `Core/` 下所有 `USER CODE` 块抓成快照（同时存一份原始文件到临时目录）；
2. 跑 CubeMX 生成；
3. 再抓一遍，逐块比对，**列出丢失和内容变化的块**。

**丢了块就返回非 0**，并把快照路径打出来供比对。

环境变量
--------

`CUBEMX_PATH` 可覆盖 CubeMX 可执行文件位置；不设则按常见安装路径探测。
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

# Windows 控制台默认 cp1252/cp936，直接 print 中文会抛 UnicodeEncodeError。
for _stream in (sys.stdout, sys.stderr):
    if hasattr(_stream, "reconfigure"):
        _stream.reconfigure(encoding="utf-8", errors="replace")

CUBEMX_CANDIDATES = (
    r"D:\STM32_HAL\STM32CubeMX\STM32CubeMX.exe",
    r"C:\STM32CubeMX\STM32CubeMX.exe",
    r"C:\Program Files\STMicroelectronics\STM32Cube\STM32CubeMX\STM32CubeMX.exe",
)

TIMEOUT_SECONDS = 900


def find_cubemx() -> Path:
    """定位 STM32CubeMX 可执行文件。"""
    env_path = os.environ.get("CUBEMX_PATH", "").strip()
    candidates = ([Path(env_path)] if env_path else []) + [Path(p) for p in CUBEMX_CANDIDATES]
    for path in candidates:
        if path.is_file():
            return path
    raise SystemExit(
        "找不到 STM32CubeMX。设环境变量 CUBEMX_PATH 指向 STM32CubeMX.exe。\n"
        f"试过: {', '.join(str(c) for c in candidates)}"
    )


def cubemx_command() -> list[str]:
    """拼出启动命令。

    安装目录里的 `STM32CubeMX.exe` 实际上是个 jar，官方启动方式是重定向到
    自带的 JRE；没有 jre 目录时退回直接执行。
    """
    exe = find_cubemx()
    java = exe.parent / "jre" / "bin" / "java.exe"
    if java.is_file():
        return [str(java), "-jar", str(exe)]
    return [str(exe)]


def cubemx_log_path() -> Path:
    """CubeMX 每次运行都会覆盖写这个日志，所以要在跑完立刻读。"""
    return Path.home() / ".stm32cubemx" / "STM32CubeMX.log"


def read_optional_messages() -> list[str]:
    """取出 CubeMX 日志里它给用户的校验结论。

    `OptionalMessage` 行是**唯一**会明说「这个参数我不认 / 这个值非法」的地方，
    而且它同时会以弹窗形式出现在屏幕上、等用户点掉才继续。
    """
    log = cubemx_log_path()
    if not log.is_file():
        return []
    try:
        text = log.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return []
    seen: list[str] = []
    for line in text.splitlines():
        idx = line.find("(OptionalMessage")
        if idx < 0:
            continue
        msg = line[idx:].strip()
        if msg and msg not in seen:
            seen.append(msg)
    return seen


def report_messages() -> None:
    """打印并解释 CubeMX 的校验消息。"""
    messages = read_optional_messages()
    if not messages:
        return
    print(f"\n!! CubeMX 给出 {len(messages)} 条校验消息：")
    for msg in messages:
        print(f"     {msg}")
    print(
        "   凡是涉及 .ioc 里真实存在的键的，都说明那个键 CubeMX 不认——\n"
        "   该外设会进入 not-ready，**生成的初始化函数会缺字段**。"
    )


def run_script(lines: list[str], work_dir: Path) -> subprocess.CompletedProcess:
    """把命令写成 .mxscript 交给 CubeMX 执行。

    三个硬性要求，违反了 CubeMX 会挂住不返回：
      · 脚本最后一行必须是 exit
      · 路径含空格必须加引号（不加会静默 KO）
      · 它弹的模态对话框必须有人点掉（脚本这边看不见，只能等）
    前两个这里已处理；第三个靠超时兜底。
    """
    if not lines or lines[-1].strip().lower() != "exit":
        raise ValueError("CubeMX 脚本必须以 exit 结尾")

    script = work_dir / "commands.mxscript"
    script.write_text("\n".join(lines) + "\n", encoding="utf-8")

    proc = subprocess.Popen(
        [*cubemx_command(), "-q", str(script)],
        cwd=work_dir,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
        creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
    )
    try:
        out, err = proc.communicate(timeout=TIMEOUT_SECONDS)
    except subprocess.TimeoutExpired:
        proc.kill()
        raise SystemExit(
            f"!! CubeMX 超过 {TIMEOUT_SECONDS} 秒没结束。\n"
            "   最常见的原因是它在弹模态对话框等用户点掉。看一眼屏幕再重跑。"
        )
    return subprocess.CompletedProcess(proc.args, proc.returncode, out, err)


def resolve_project(raw: str) -> tuple[Path, Path]:
    """把「工程根目录」或「.ioc 路径」解析成 (工程根, .ioc 路径)。"""
    path = Path(raw).resolve()
    if path.is_file():
        if path.suffix != ".ioc":
            raise SystemExit(f"不是 .ioc 文件: {path}")
        return path.parent, path
    ioc = path / f"{path.name}.ioc"
    if not ioc.is_file():
        raise SystemExit(f"在 {path} 里找不到 {path.name}.ioc")
    return path, ioc


def quoted(path: Path) -> str:
    return f'"{path}"'


def parse_ioc(path: Path) -> dict[str, str]:
    """把 .ioc 读成扁平键值表（跳过注释行）。"""
    entries: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8-sig").splitlines():
        line = line.rstrip("\r")
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        entries[key.strip()] = value.strip()
    return entries


# 匹配 /* USER CODE BEGIN <名字> */ ... /* USER CODE END <名字> */
# 名字里可能有空格（CubeMX 自己用的是 USART1_Init 0 这种），所以用 \S+ 之外的写法。
_USER_BLOCK_RE = re.compile(
    r"/\* USER CODE BEGIN ([^*]*?) \*/(.*?)/\* USER CODE END \1 \*/",
    re.DOTALL,
)

# 生成过程会重写这些后缀的文件；其余（.ioc、CMakeLists 等）不在比对范围内。
_USER_CODE_SUFFIXES = (".c", ".h")


def snapshot_user_code(root: Path) -> dict[str, str]:
    """抓取 Core/ 下所有 USER CODE 块。

    返回值把块名映射到**去掉首尾空白的正文**，键形如
    ``Core/Src/main.c:USART1_Init 0``。空块也会被抓到（正文为空串），
    因为"块还在、只是被清空了"和"块整个没了"是两种不同的故障。
    """
    blocks: dict[str, str] = {}
    core = root / "Core"
    if not core.is_dir():
        return blocks

    for path in sorted(core.rglob("*")):
        if path.suffix not in _USER_CODE_SUFFIXES or not path.is_file():
            continue
        try:
            text = path.read_text(encoding="utf-8-sig", errors="replace")
        except OSError:
            continue
        rel = path.relative_to(root).as_posix()
        for name, body in _USER_BLOCK_RE.findall(text):
            blocks[f"{rel}:{name.strip()}"] = body.strip()

    return blocks


def compare_user_code(before: dict[str, str],
                      after: dict[str, str]) -> tuple[list[str], list[str]]:
    """比对两次快照，返回 (丢失的块, 内容变化的块)。

    新增的块不算问题——那正是新加外设的正常结果（CubeMX 会给它生成新的空块）。
    """
    lost = [k for k in sorted(before) if k not in after]
    changed = [k for k in sorted(before)
               if k in after and before[k] != after[k]]
    return lost, changed


def hash_tree(directory: Path) -> dict[str, str]:
    """把目录下所有文件映射成「相对路径 -> 内容 sha256」。

    用来判断生成前后**内容**变没变。刻意不用 mtime：实测 CubeMX 在
    「生成结果与磁盘上已有的内容完全一致」时**不会重写文件**，
    mtime 保持原样——拿它判「有没有生成」会误报。
    """
    digests: dict[str, str] = {}
    if not directory.is_dir():
        return digests

    for path in sorted(directory.rglob("*")):
        if not path.is_file():
            continue
        try:
            digests[path.relative_to(directory).as_posix()] = \
                hashlib.sha256(path.read_bytes()).hexdigest()
        except OSError:
            continue

    return digests


def script_results(stdout: str, commands: list[str]) -> list[tuple[str, str]]:
    """把 CubeMX 的回显配成 (命令, 结果) 列表。

    CubeMX 在脚本模式下会把每条命令连同结果交替回显，中间还混着别的输出
    （日志行、`Invalid condition id` 之类的噪音）。所以不能按行号硬对齐——
    按命令原文定位，再往后找第一个恰好是 OK/KO 的行。

    找不到回显时结果记作 "?"，调用方要把它和 KO 区分开处理。
    """
    results: list[tuple[str, str]] = []
    for cmd in commands:
        idx = stdout.find(cmd)
        if idx < 0:
            results.append((cmd, "?"))
            continue
        answer = "?"
        for line in stdout[idx + len(cmd):].splitlines():
            token = line.strip()
            if token in ("OK", "KO"):
                answer = token
                break
        results.append((cmd, answer))
    return results


def recipe_lines(path: Path) -> list[str]:
    """读配方文件：跳过空行与 `#` 注释，并挡住脚本自带的命令。"""
    lines: list[str] = []
    for raw in path.read_text(encoding="utf-8-sig").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line.lower() == "exit" or line.lower().startswith("config "):
            raise SystemExit(
                f"配方文件里不要写 `{line}`；本脚本会自己加 config load / saveas / exit。"
            )
        lines.append(raw.rstrip("\r\n"))
    return lines


def cmd_apply(args: argparse.Namespace) -> int:
    root, ioc = resolve_project(args.project_dir)
    recipe = Path(args.recipe).resolve()
    if not recipe.is_file():
        raise SystemExit(f"找不到配方文件: {recipe}")

    commands = recipe_lines(recipe)
    if not commands:
        raise SystemExit("配方文件里没有任何命令。")
    print(f"配方: {recipe}（{len(commands)} 条命令）")

    with tempfile.TemporaryDirectory(prefix="cubemx-apply-") as tmp:
        work = Path(tmp)
        staged = work / ioc.name
        result = run_script(
            [f"config load {quoted(ioc)}", *commands, f"config saveas {quoted(staged)}", "exit"],
            work,
        )
        if not staged.is_file():
            print((result.stdout or "")[-3000:])
            raise SystemExit("!! CubeMX 没有产出新 .ioc。")

        if any(line.strip() == "KO" for line in (result.stdout or "").splitlines()):
            print("!! CubeMX 回了 KO（多半是语法或路径问题）：")
            print((result.stdout or "")[-2000:])
            return 1

        backup = ioc.with_suffix(".ioc.bak")
        shutil.copy2(ioc, backup)
        shutil.copy2(staged, ioc)

    print(f"已更新: {ioc}")
    print(f"备份:   {backup}")
    for line in commands:
        print(f"  {line}")
    report_messages()
    print("\n下一步：python tools/cubemx_apply.py roundtrip <工程根>")
    return 0


# 这些是 CubeMX 每次都会重算/重排的结构键，被改写属正常，不算问题。
_BENIGN_PREFIXES = ("Mcu.Pin", "Mcu.PinsNb", "Mcu.IP", "Mcu.IPNb", "ProjectManager.")
_BENIGN_NAMES = {"PinOutPanel.RotationAngle", "KeepUserPlacement", "Mcu.UserConstants",
                 "Mcu.ThirdPartyNb"}
# 派生的频率键由 CubeMX 按当前配置重算，本身就该变。
_DERIVED_SUFFIXES = ("Freq_Value", "FreqValue", "Freq_VALUE")


def _is_benign(key: str) -> bool:
    if key.endswith(".IPParameters"):
        return True
    return key in _BENIGN_NAMES or key.startswith(_BENIGN_PREFIXES)


def _is_derived(key: str) -> bool:
    return any(key.endswith(s) for s in _DERIVED_SUFFIXES)


def cmd_roundtrip(args: argparse.Namespace) -> int:
    root, ioc = resolve_project(args.target)

    with tempfile.TemporaryDirectory(prefix="cubemx-rt-") as tmp:
        work = Path(tmp)
        normalized = work / "normalized.ioc"
        result = run_script(
            [f"config load {quoted(ioc)}",
             f"project path {quoted(work)}",
             f"config saveas {quoted(normalized)}",
             "exit"],
            work,
        )
        if not normalized.is_file():
            print((result.stdout or "")[-3000:])
            raise SystemExit("CubeMX 没能完成往返。")
        # 必须在 with 块**内**读完：退出时临时目录连同 normalized.ioc 一起被删。
        before = parse_ioc(ioc)
        after = parse_ioc(normalized)
        normalized_path = str(normalized)

    dropped = [k for k in before if k not in after]
    changed = [(k, before[k], after[k]) for k in before if k in after and before[k] != after[k]]
    hard_dropped = [k for k in dropped if not _is_benign(k) and not _is_derived(k)]
    hard_changed = [c for c in changed if not _is_benign(c[0]) and not _is_derived(c[0])]

    print(f"源文件:   {ioc}")
    print(f"归一化后: {normalized_path}")
    print(f"键数: {len(before)} -> {len(after)}\n")

    if hard_dropped:
        print("!! CubeMX 丢弃了这些键（写进去的参数没生效）:")
        for key in hard_dropped:
            print(f"     - {key} = {before[key]}")
        print(
            "\n   诊断顺序：\n"
            "   ① 如果这是**新加的一整个外设**，被丢的多半是它的全部参数。原因不是名字错，\n"
            "      而是外设没被真正激活——不要猜名字，回去用 `set mode` 激活它。\n"
            "   ② 如果外设本来就在工程里、只是加参数，那才可能是名字不对。"
        )

    if hard_changed:
        print("!! CubeMX 改写了这些键的值:")
        for key, was, now in hard_changed:
            print(f"     ~ {key}: {was!r} -> {now!r}")

    if not (hard_dropped or hard_changed):
        print("OK: 写的参数全部被 CubeMX 原样保留了。")
        print("    （这只证明「参数被接受」，还要去生成的 .c 里确认「代码被生成」。）")

    report_messages()
    return 1 if (hard_dropped or hard_changed) else 0


def cmd_regenerate(args: argparse.Namespace) -> int:
    root, ioc = resolve_project(args.project_dir)

    if not (root / "Core").is_dir():
        raise SystemExit(
            f"{root} 里没有 Core/，这不像是一个已经生成过的工程。\n"
            "（空文件夹要用新的工程来生成，见 docs/walkthrough.md 阶段 2~5。）"
        )

    entries = parse_ioc(ioc)
    project_name = entries.get("ProjectManager.ProjectName", "").strip() or ioc.stem
    toolchain = entries.get("ProjectManager.TargetToolchain", "").strip() or "CMake"

    # ---- 1) 生成前快照 --------------------------------------------------
    before = snapshot_user_code(root)
    if not before:
        raise SystemExit(
            "在 Core/ 下没抓到任何 USER CODE 块——这很反常，先确认工程布局对不对，"
            "不要在没快照的情况下生成。"
        )

    keep = Path(tempfile.mkdtemp(prefix="cubemx-regen-before-"))
    shutil.copytree(root / "Core", keep / "Core")
    before_files = hash_tree(root / "Core")
    print(f"生成前: {len(before)} 个 USER CODE 块，Core/ 下 {len(before_files)} 个文件")
    print(f"        原始文件已备份到 {keep / 'Core'}")

    # ---- 2) 生成 --------------------------------------------------------
    commands = [
        f"config load {quoted(ioc)}",
        f"project name {project_name}",
        f"project toolchain {toolchain}",
        f"project path {quoted(root.parent)}",
        "project generate",
        "exit",
    ]

    print(f"生成中: project={project_name} toolchain={toolchain} "
          f"path={root.parent}")
    with tempfile.TemporaryDirectory(prefix="cubemx-regen-") as work:
        result = run_script(commands, Path(work))

    stdout = result.stdout or ""
    results = script_results(stdout, commands)

    # ⚠️ 别拿单条命令的返回码当成败判据。
    # 实测（本机 2026-10-11，路径含空格）：`project path` 会回 KO，
    # 但 `project generate` 照常 OK、生成落点完全正确。
    # 所以这里只把 KO 分成「已知无害」和「意外」两类做提示，
    # **真正的判据是内容**（下面第 3 步）——看产物，不看返回码。
    benign_ko = [cmd for cmd, res in results
                 if res == "KO" and cmd.startswith("project path")]
    fatal_ko = [cmd for cmd, res in results
                if res == "KO" and not cmd.startswith("project path")]

    for cmd in benign_ko:
        print(f"   注意: `{cmd}` 回了 KO —— 实测这条不影响生成落点，"
              "按已知无害处理，成败以下面的内容比对为准。")

    if fatal_ko:
        print("\n!! CubeMX 回了 KO：")
        for cmd in fatal_ko:
            print(f"     {cmd}")
        print(stdout[-2000:])
        report_messages()
        return 1

    # ---- 3) 内容比对：Core/ 下到底变没变 --------------------------------
    # 判据只能是**内容**：实测 CubeMX 在生成结果与磁盘上已有内容完全一致时
    # 不重写文件（mtime 不变），用 mtime 判「有没有生成」会误报。
    after_files = hash_tree(root / "Core")
    added = sorted(k for k in after_files if k not in before_files)
    removed = sorted(k for k in before_files if k not in after_files)
    modified = sorted(k for k in before_files
                      if k in after_files and before_files[k] != after_files[k])

    if added or modified:
        print("\nCore/ 下的变化：")
        for key in added:
            print(f"     + {key}")
        for key in modified:
            print(f"     ~ {key}")
    else:
        print("\nCore/ 下没有任何内容变化（生成结果与磁盘上已有的一致）。")
        print("   如果你这次改过 .ioc、且期望看到新外设的初始化代码，"
              "这里一点变化都没有就值得怀疑。")

    if removed:
        print("\n!! 这些文件在生成后**被删掉了**：")
        for key in removed:
            print(f"     - {key}")
        print(f"   生成前的副本在 {keep / 'Core'}，先别动工作区。")

    # ---- 4) 比对 USER CODE ----------------------------------------------
    after = snapshot_user_code(root)
    lost, changed = compare_user_code(before, after)

    print(f"\n生成后: {len(after)} 个 USER CODE 块（{len(before)} -> {len(after)}）")

    if lost:
        print("\n!! 这些块在生成后**整个消失了**：")
        for key in lost:
            print(f"     - {key}")
        print(f"   生成前的原文在 {keep / 'Core'}，可以逐个恢复。")

    if changed:
        print("\n!! 这些块的**正文被改写了**：")
        for key in changed:
            print(f"     ~ {key}")

    if lost or changed:
        print(
            "\n   别急着把代码重写一遍——那只治症状。诊断顺序：\n"
            "   ① 有没有人绕过 CubeMX 手改过 .ioc？（头号嫌疑，但**因果未经证实**）\n"
            "   ② 有没有改过 Core/ 里 USER CODE 块之外的内容？\n"
            "   ③ 以上都不是，再怀疑工具本身，并记下可复现步骤。\n"
            "   详见 docs/walkthrough.md 阶段 5。"
        )
        report_messages()
        return 1

    if removed:
        print("\n   生成不该删文件。先看 .ioc 里是不是少了哪个外设，"
              "再决定要不要从上面的备份恢复。")
        report_messages()
        return 1

    print("OK: 所有 USER CODE 块都原样幸存。")
    print("   下一步：去生成的 .c 里逐项找证据（别只看 .ioc）——"
          "见 docs/architecture.md 的「验证纪律」。")
    print("   再下一步：cmake --build build/Debug")
    report_messages()
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("apply", help="按配方文件改配置，写回工程的 .ioc")
    p.add_argument("project_dir", help="工程根目录（或 .ioc 路径）")
    p.add_argument("recipe", help="配方文件，每行一条 CubeMX 命令")
    p.set_defaults(func=cmd_apply)

    p = sub.add_parser("roundtrip", help="验证改动是否被 CubeMX 原样接受")
    p.add_argument("target", help="工程根目录（或 .ioc 路径）")
    p.set_defaults(func=cmd_roundtrip)

    p = sub.add_parser("regenerate",
                       help="就地重新生成代码（apply 之后必须跑这一步）")
    p.add_argument("project_dir", help="工程根目录（或 .ioc 路径）")
    p.set_defaults(func=cmd_regenerate)

    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
