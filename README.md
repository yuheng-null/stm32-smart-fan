# smart_fan — 智能温控风扇系统

基于 STM32F407VET6 的温控风扇。

当前进度：**工具链 → 板子 → 串口**链路已打通并实测验证（时钟 168 MHz、串口中断收发）；
**OLED / NTC 测温 / 按键**三路驱动也已写完并实测通过（见下面的「验证外设」）。
温控算法与风扇调速尚未开始——`app/app_main.c` 目前是这三个外设的测试台，
下一步把温控状态机替换掉 `app_main_run()` 里的循环。

## 硬件

| 项 | 配置 |
|---|---|
| MCU | STM32F407VET6（LQFP100，512 KB Flash，128 KB RAM + 64 KB CCMRAM） |
| 主频 | 168 MHz（HSE 8 MHz 晶振 → PLLM=8 / PLLN=336 / PLLP=2） |
| 调试 | ST-Link，SWD（PA13 = SWDIO，PA14 = SWCLK） |
| 串口 | USART1，115200-8-N-1，**PA9 = TX / PA10 = RX**，接 CH340 USB 转串口 |
| OLED | SSD1306 128×64，**软件 I2C：PB6 = SCL / PB7 = SDA**，模块板载 4.7 kΩ 上拉 |
| 测温 | LM393 型 NTC 模块的 **AO → PA0（ADC1_IN0）**，模块 VCC 必须接 **3.3V** |
| 按键 | 两个轻触按键，另一端接 GND：**K1 = PC0 / K2 = PC1**，上拉输入，按下为低 |

CH340 与板子的接法（**必须交叉，且必须共地**）：

```
CH340 TX  ──►  PA10 (MCU RX)
CH340 RX  ◄──  PA9  (MCU TX)
CH340 GND ───  GND
```

完整的**引脚分配表、每个外设的接线图、以及电气上的硬性要求**
（NTC 模块为什么必须 3.3V、I2C 的上拉从哪来、ADC 采样时间为什么取 480 周期）
都在 [`docs/hardware.md`](docs/hardware.md)。

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

## 验证

固件每 200 ms 输出一行状态，同时把同样内容画到 OLED 上；上电时先做一次 I2C 总线扫描。

```bash
python tools/capture_serial.py                    # 复位板子并抓取
python tools/capture_serial.py --seconds 40       # 抓久一点
python tools/capture_serial.py --echo "PING"      # 发数据出去，验证 RX 回显
```

预期输出：

```
==== smart_fan boot ====
SYSCLK = 168000000 Hz
外设: OLED(SSD1306, 软 I2C PB6/PB7) + NTC(PA0/ADC1_IN0) + 按键(PC0/PC1)
...
OLED: probing 0x3C/0x3D ...
I2C scan: 1 device(s) 0x3C
OLED: OK
[  466 ms] T = 23.8 C  raw = 2103  R = 10.55 kohm  K1 = 0(-)  K2 = 0(-)  drop = 0  err = 0  status = 0
```

每个字段怎么读：

| 字段 | 含义 |
|---|---|
| `SYSCLK` | 时钟树对不对。**这行本身就是独立验证**：主频算错时串口波特率跟着错，整屏都是乱码，你根本读不到这行 |
| `I2C scan` / `OLED: OK` | 屏有没有应答、用了哪个地址 |
| `T` | 换算出的温度（**未校准，只看趋势**） |
| `raw` | ADC 原始计数（0~4095）。**这是没经过任何假设的数字**，校准以它为准 |
| `R` | 由 `raw` 反算出的 NTC 阻值（kΩ），用来和万用表实测值对照 |
| `K1` / `K2` | 累计按下次数；括号里 `*` 表示此刻正按着 |
| `drop` | 接收缓冲满而丢弃的字节数 |
| `err` | 串口出错次数（帧错误 / 噪声 / 溢出） |
| `status` | `temperature_from_adc_raw()` 的返回码，`0` 即正常 |

### 三路外设各自的判据

| 外设 | 静态：证明「采到了」 | 动态：证明「真的在响应」 |
|---|---|---|
| OLED | `I2C scan` 有 `0x3C`（或 `0x3D`）且 `OLED: OK` | **必须肉眼确认屏上有字**——`OK` 只证明命令被 ACK，不证明画对了 |
| NTC | `raw` 落在合理区间（室温下约 1900~2200） | 捏住模块上的热敏电阻十几秒：`raw` 应**下降**、`T` 上升，松手后回落 |
| 按键 | 未按时计数不动 | 按一下计数 **+1**（多按一次不会 +2），按下期间显示 `(*)` |

「静态通」和「动态对」是两件事：命令被 ACK 不等于内容画对了，
ADC 采到数不等于方向没接反。**两类都要过**。

### 排障：`I2C scan` 是第一入口

它能把三种原因一次分开：

- **一个地址都没有** → 接线 / 供电 / 上拉问题（`SCL`/`SDA` 接反也在这里暴露）；
- **只有 `0x3C`（或 `0x3D`）** → 正常；
- **出现别的地址** → 总线上挂了非预期器件。

`drop` / `err` 是判断串口链路最直接的指标：**接线松、没共地、波特率不匹配时
`err` 会持续上涨**，比看现象猜接线快得多。

（若串口助手占着 COM 口，脚本会报 `FAILED_TO_OPEN` —— 先关掉它。）

> ⚠️ **温度目前不是准确值。** NTC 的 `R25` 和 `B` 是按模块型号**假定**的
> （10 kΩ、B=3950），未实测。校准方法写在
> [`module/temperature/temperature.h`](module/temperature/temperature.h) 顶部。

## 改配置（时钟 / 外设）

**外设、引脚、时钟树一律通过 CubeMX 改，不要手写 `.ioc`。**

`.ioc` 是 CubeMX 内部状态的序列化：手写 `Mcu.IPn=TIM1` 这类「该外设已激活」的**记录**，
CubeMX 重建不出对应状态就整块**静默丢弃**——不报错、不警告，参数连同记录一起没了。

所以本工程把配置改动都写成 [`cube-recipes/`](cube-recipes/) 下的配方文件，
用仓库自带的 `tools/cubemx_apply.py` 喂给 CubeMX 自己的脚本模式：

```bash
python tools/cubemx_apply.py apply     . "cube-recipes/r1-hse-usart-swd.txt"
python tools/cubemx_apply.py apply     . "cube-recipes/r2-时钟树.txt"   # 必须在 r1 之后
python tools/cubemx_apply.py apply     . "cube-recipes/r3-oled-ntc-key.txt"
python tools/cubemx_apply.py roundtrip .                               # 验证改动
```

`apply` **只改 `.ioc`，不生成代码**。要出代码得再跑一次 CubeMX 的生成，
在 CubeMX GUI 里点 GENERATE CODE，或脚本模式（注意 `project path` 给的是**父目录**）：

```text
config load <工程根>/smart_fan.ioc
project name smart_fan
project toolchain CMake
project path <工程根>/..
project generate
exit
```

生成前先把 `Core/` 下所有 `USER CODE` 块快照下来，生成后逐块核对是否幸存——
CubeMX 靠「读回这些块」保留用户代码，而这个机制**偶尔静默失效**（细节见
[`docs/walkthrough.md`](docs/walkthrough.md) 阶段 5）。

> ⚠️ **时钟树必须分两遍跑**，原因写在 [`cube-recipes/r2-时钟树.txt`](cube-recipes/r2-时钟树.txt)
> 顶部：`RCC.HSE_VALUE` 只在 CubeMX `config load` 那一刻被时钟模型读取，写在同一遍里
> 会导致「命令全 OK、键也落盘、roundtrip 也报原样保留，但主频算错」。

改完配置的验收要分四层做，缺一层就会出现「全绿但错」——
见 [`docs/architecture.md`](docs/architecture.md) 的「验证纪律」。

## 文档

| 文件 | 内容 |
|---|---|
| [`docs/hardware.md`](docs/hardware.md) | **引脚分配表、接线图、电气要求**（3.3V 的理由、上拉来源、采样时间依据） |
| [`docs/architecture.md`](docs/architecture.md) | 分层规则、依赖方向、CubeMX 管辖边界、验证纪律、F4 与 F1 的平台差异 |
| [`docs/walkthrough.md`](docs/walkthrough.md) | **从空文件夹到跑通的完整操作记录**，每步附验收标准 |
| [`driver/README.md`](driver/README.md) | 驱动层的边界：`bus/` 与 `device/` 各自能碰什么 |
| [`module/README.md`](module/README.md) | 业务层的硬性约束与命名约定 |
| [`cube-recipes/`](cube-recipes/) | 配置配方，含踩坑说明 |

## 目录结构

```
smart_fan/
├── Core/            CubeMX 生成：HAL 适配、main.c、中断向量
├── Drivers/         CubeMX 生成：HAL 驱动 + CMSIS
├── cmake/           CubeMX 生成：CMake 胶水
├── interface/       硬件抽象契约（只有声明，无实现、无厂商头文件）
├── driver/          驱动
│   ├── bus/         总线实现（软件 I2C）——贴 HAL 的那一层
│   └── device/      器件驱动（SSD1306）——纯可移植，不含任何 STM32 符号
├── bsp/             板级绑定：把契约与驱动绑到具体引脚/外设
├── module/          业务逻辑（温度换算、风扇控制）
├── app/             启动编排、状态机
├── cube-recipes/    下达给 CubeMX 的配置配方（改配置的唯一入口）
├── docs/            架构说明 + 完整开发过程记录
├── tools/           cubemx_apply.py（配置下达）、capture_serial.py（串口验证）
└── .vscode/         clangd / CMake Tools / 断点调试配置
```

**VSCode 打开的就是这一层目录本身**，不要用外层目录当工作区——
ST 的工具链假设「工作区根 == CMake 工程目录」，错开会让它
从代码索引到调试以三种不同面目失败。

分层与依赖方向（`APP → MODULE → INTERFACE`，`BSP → DRIVER → PLATFORM`），
以及「为什么把厂商代码留在 `Core/`+`Drivers/` 而不是搬进 `platform/`」，
都写在 [`docs/architecture.md`](docs/architecture.md)。

## 许可

本项目自有代码以 [MIT 协议](LICENSE) 开源，Copyright (c) 2026 yuheng-null。

仓库内含第三方代码，**不适用 MIT**，各自遵循上游协议：

| 路径 | 版权方 | 协议 |
|---|---|---|
| `Drivers/STM32F4xx_HAL_Driver/` | STMicroelectronics | BSD-3-Clause |
| `Drivers/CMSIS/` | ARM Limited | Apache-2.0 |
| `driver/device/ssd1306/ssd1306_fonts.*` | [afiskon/stm32-ssd1306](https://github.com/afiskon/stm32-ssd1306) | MIT |

完整归属说明见 [NOTICE](NOTICE)。
