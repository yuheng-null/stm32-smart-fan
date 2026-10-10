# smart_fan — 智能温控风扇系统

基于 STM32F407VET6 的温控风扇。

当前进度：**工具链 → 板子 → 串口**链路已打通并实测验证（时钟 168 MHz、串口中断收发）。
温控算法与风扇调速尚未开始。

## 硬件

| 项 | 配置 |
|---|---|
| MCU | STM32F407VET6（LQFP100，512 KB Flash，128 KB RAM + 64 KB CCMRAM） |
| 主频 | 168 MHz（HSE 8 MHz 晶振 → PLLM=8 / PLLN=336 / PLLP=2） |
| 调试 | ST-Link，SWD（PA13 = SWDIO，PA14 = SWCLK） |
| 串口 | USART1，115200-8-N-1，**PA9 = TX / PA10 = RX**，接 CH340 USB 转串口 |

CH340 与板子的接法（**必须交叉，且必须共地**）：

```
CH340 TX  ──►  PA10 (MCU RX)
CH340 RX  ◄──  PA9  (MCU TX)
CH340 GND ───  GND
```

## 编译

工具链不在系统 PATH 上，每个新终端都要先导：

```bash
export PATH="$HOME/AppData/Local/stm32cube/bundles/gnu-tools-for-stm32/14.3.1+st.2/bin:$HOME/AppData/Local/stm32cube/bundles/cmake/4.3.1+st.1/bin:$HOME/AppData/Local/stm32cube/bundles/ninja/1.13.2+st.1/bin:$PATH"

cmake --preset Debug
cmake --build build/Debug
```

（版本号用 `ls ~/AppData/Local/stm32cube/bundles/<工具>` 现查，别硬记。）

## 烧录

```bash
B="$HOME/AppData/Local/stm32cube/bundles/programmer/2.23.0/bin"

# 先探测，核对 Device ID / NVM size / 电压
"$B/STM32_Programmer_CLI.exe" -c port=SWD mode=UR

# 写入 + 校验 + 复位运行，看到 Download verified successfully 才算成功
"$B/STM32_Programmer_CLI.exe" -c port=SWD mode=UR -w build/Debug/smart_fan.elf -v -rst
```

## 验证串口

```bash
python tools/capture_serial.py                  # 复位板子，抓开机横幅 + 心跳
python tools/capture_serial.py --echo "PING"    # 发数据出去，验证 RX 回显
```

脚本会打印每条消息的到达时刻，并算出心跳间隔。**心跳间隔是独立于代码的时钟验证**：
固件按 1000 tick 排心跳，间隔明显偏离 1.000 s 就说明实际主频与配置不符。

预期输出：

```
==== smart_fan boot ====
SYSCLK = 168000000 Hz
USART1 = 115200-8-N-1, PA9=TX PA10=RX
...
[heartbeat 1] tick = 1034 ms  rx = 0  drop = 0  err = 0
[heartbeat 2] tick = 2034 ms  rx = 0  drop = 0  err = 0
```

`drop` / `err` 是判断链路健康最直接的指标：**接线松、没共地、波特率不匹配时 `err`
会持续上涨**，比看现象猜接线快得多。

（若串口助手开着，脚本会报 `FAILED_TO_OPEN` —— 先关掉它。）

## 改配置（时钟 / 外设）

**外设、引脚、时钟树一律通过 CubeMX 改，不要手写 `.ioc`。**

`.ioc` 是 CubeMX 内部状态的序列化：手写 `Mcu.IPn=TIM1` 这类「该外设已激活」的**记录**，
CubeMX 重建不出对应状态就整块**静默丢弃**——不报错、不警告，参数连同记录一起没了。

所以本工程把配置改动都写成 [`cube-recipes/`](cube-recipes/) 下的配方文件，
用仓库自带的 `tools/cubemx_apply.py` 喂给 CubeMX 自己的脚本模式：

```bash
python tools/cubemx_apply.py apply     . "cube-recipes/r1-hse-usart-swd.txt"
python tools/cubemx_apply.py apply     . "cube-recipes/r2-时钟树.txt"   # 必须在 r1 之后
python tools/cubemx_apply.py roundtrip .                               # 验证改动
```

> ⚠️ **时钟树必须分两遍跑**，原因写在 [`cube-recipes/r2-时钟树.txt`](cube-recipes/r2-时钟树.txt)
> 顶部：`RCC.HSE_VALUE` 只在 CubeMX `config load` 那一刻被时钟模型读取，写在同一遍里
> 会导致「命令全 OK、键也落盘、roundtrip 也报原样保留，但主频算错」。

改完配置的验收要分四层做，缺一层就会出现「全绿但错」——
见 [`docs/architecture.md`](docs/architecture.md) 的「验证纪律」。

## 文档

| 文件 | 内容 |
|---|---|
| [`docs/architecture.md`](docs/architecture.md) | 分层规则、依赖方向、CubeMX 管辖边界、验证纪律 |
| [`docs/walkthrough.md`](docs/walkthrough.md) | **从空文件夹到跑通的完整操作记录**，每步附验收标准 |
| [`cube-recipes/`](cube-recipes/) | 配置配方，含踩坑说明 |

## 目录结构

```
smart_fan/
├── Core/            CubeMX 生成：HAL 适配、main.c、中断向量
├── Drivers/         CubeMX 生成：HAL 驱动 + CMSIS
├── cmake/           CubeMX 生成：CMake 胶水
├── interface/       硬件抽象契约（只有声明，无实现、无厂商头文件）
├── bsp/             板级实现：把契约绑到具体外设
├── module/          业务逻辑（温控算法、风扇控制）
├── app/             启动编排、状态机
├── cube-recipes/    下达给 CubeMX 的配置配方（改配置的唯一入口）
├── docs/            架构说明 + 完整开发过程记录
├── tools/           cubemx_apply.py（配置下达）、capture_serial.py（串口验证）
└── .vscode/         clangd / CMake Tools / 断点调试配置
```

**VSCode 打开的就是这一层目录本身**，不要用外层目录当工作区——
ST 的工具链假设「工作区根 == CMake 工程目录」，错开会让它
从代码索引到调试以三种不同面目失败。

依赖方向 `APP → MODULE → INTERFACE → BSP → PLATFORM`，详见
[`docs/architecture.md`](docs/architecture.md)。那里也写明了本工程
为什么把厂商代码留在 `Core/`+`Drivers/` 而不是搬进 `platform/`。
