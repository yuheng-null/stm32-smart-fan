# 硬件：引脚分配与接线

本文件是**引脚这件事的唯一说明处**。代码里凡是引用引脚的宏
（`KEY1_Pin` / `OLED_SCL_GPIO_Port` …）都由 CubeMX 从 `.ioc` 生成，
改接线要改 `.ioc`（走 [`cube-recipes/`](../cube-recipes/)），不要改代码。

## 主控与时钟

| 项 | 值 | 说明 |
|---|---|---|
| MCU | STM32F407VET6 | LQFP100，512 KB Flash，128 KB RAM + 64 KB CCMRAM |
| 封装 | LQFP100 | CubeMX 里 `Mcu.Package=LQFP100` |
| HSE | 8 MHz 晶振 | 接 **PH0 = OSC_IN / PH1 = OSC_OUT** |
| 主频 | 168 MHz | PLLM=8 / PLLN=336 / PLLP=2 / PLLQ=7，AHB /1、APB1 /4、APB2 /2 |
| APB1 | 42 MHz | 定时器时钟 84 MHz |
| APB2 | 84 MHz | 定时器时钟 168 MHz；**ADC 时钟源 PCLK2 = 84 MHz** |
| FLASH 等待周期 | 5 | 168 MHz 下必须 ≥5，CubeMX 已按主频算好 |
| 调试 | SWD | **PA13 = SWDIO / PA14 = SWCLK**，用物理引脚而不是 `SYS.Debug` 那个键 |

## 引脚分配表

| 引脚 | 信号 | 方向/模式 | 接什么 | 代码里的宏 | 谁配的 |
|---|---|---|---|---|---|
| PH0 | RCC_OSC_IN | 外部晶振 | 8 MHz 晶振 | — | CubeMX |
| PH1 | RCC_OSC_OUT | 外部晶振 | 8 MHz 晶振 | — | CubeMX |
| PA13 | SYS_JTMS-SWDIO | 复用 | ST-Link SWDIO | — | CubeMX |
| PA14 | SYS_JTCK-SWCLK | 复用 | ST-Link SWCLK | — | CubeMX |
| PA9 | USART1_TX | 复用 推挽 | CH340 的 **RX** | — | CubeMX |
| PA10 | USART1_RX | 复用 浮空 | CH340 的 **TX** | — | CubeMX |
| **PB6** | GPIO_Output | **开漏** + 上拉 + 高速 | OLED **SCL** | `OLED_SCL_Pin` / `OLED_SCL_GPIO_Port` | CubeMX，驱动会再配一遍 |
| **PB7** | GPIO_Output | **开漏** + 上拉 + 高速 | OLED **SDA** | `OLED_SDA_Pin` / `OLED_SDA_GPIO_Port` | 同上 |
| **PA0** | ADC1_IN0 | **模拟输入** | NTC 模块 **AO** | — （ADC 通道不用宏） | CubeMX |
| **PC0** | GPIO_Input | **上拉输入** | 按键 **K1** 的一端 | `KEY1_Pin` / `KEY1_GPIO_Port` | CubeMX |
| **PC1** | GPIO_Input | **上拉输入** | 按键 **K2** 的一端 | `KEY2_Pin` / `KEY2_GPIO_Port` | CubeMX |

> `PA0` 在 CubeMX 里被规范化成 `PA0-WKUP`（它带唤醒功能），这是同一个引脚。

CubeMX 对 **ADC 通道不生成引脚宏**（不像 GPIO 会生成 `KEY1_Pin` 那样），
所以 `ADC_CHANNEL_0` 与 PA0 的对应关系只写在
[`bsp/bsp_adc.h`](../bsp/bsp_adc.h) 的注释里——那正是 BSP 层的职责：
「这块板子上，什么接在什么脚上」。

## 接线

### 调试串口（USART1 ↔ CH340）

```
CH340 TX  ──►  PA10 (MCU RX)
CH340 RX  ◄──  PA9  (MCU TX)
CH340 GND ───  GND
```

**TX/RX 必须交叉，且必须共地**。不共地时常见现象是「能发不能收」或 `err` 计数持续上涨。

### OLED（SSD1306，四针模块）

```
模块丝印      接到
---------     --------------------------------
GND      ──   GND
VCC      ──   3V3      （不要接 5V，理由见下）
SCL      ──   PB6
SDA      ──   PB7
```

四针模块的丝印顺序各家不同，**接线前按丝印核对，不要按位置猜**。
模块板载 4.7 kΩ 上拉已在 SCL/SDA 上，无需外接。

### NTC 测温（LM393 型四针模块）

```
模块丝印      接到
---------     --------------------------------
VCC      ──   3V3      ⚠️ 必须 3.3V
GND      ──   GND
DO       ──   （不用；那是 LM393 比较器的数字输出）
AO       ──   PA0
```

模块上 AO 取的是**固定电阻与 NTC 串联分压的中点**：

```
   3V3 ──[ R_FIXED ]──┬── AO ──► PA0
                      │
                  [ R_NTC ]
                      │
                     GND
```

温度升高 → `R_NTC` 变小 → AO 电压下降。实测确认了这个方向
（捏住热敏电阻时 `raw` 从 2105 掉到 1733）。

### 按键（两个轻触开关）

```
K1:  PC0 ──┬── 开关 ── GND        K2:  PC1 ──┬── 开关 ── GND
           │                                 │
        （芯片内部上拉）                  （芯片内部上拉）
```

另一端接 GND，引脚配上拉输入，所以**按下读到低电平**。
若实际接法相反（另一端接 VCC），要同时改两处：
`bsp/bsp_key.c` 里的 `BSP_KEY_ACTIVE_LEVEL` 和 `.ioc` 里这两个引脚的上下拉。

## 为什么 NTC 模块必须是 3.3V

两条独立的理由，任何一条都足以否决 5V：

**① 量程会被削顶。** ADC 的参考电压 `VREF+` 是 3.3V。模块若用 5V 供电，
AO 输出 0~5V，高过 3.3V 的部分**全部被压平在满量程 4095**——
温度偏低的那一大段读数完全一样，等于那段没有区分度。

**② 过压。** 数据手册的引脚表里 PA0 标着 FT（5V 耐受），但那条脚注写的是
「FT = 5 V tolerant **except when in analog mode**」。
PA0 一旦被配成 ADC 的模拟输入**就不再是 5V 耐受**，输入上限是 VDDA（绝对最大 3.6V）。
5V 会经保护二极管倒灌。

3.3V 还带来一个附带好处：模块 VCC 与 ADC 的 `VREF+` 取自同一条轨，
换算 NTC 阻值时**两者相消**（推导见
[`module/temperature/temperature.h`](../module/temperature/temperature.h)），
于是 3.3V 轨实际是 3.26V 还是 3.34V 都不影响结果。

## ADC 采样时间为什么取 480 周期

NTC 分压的中点，源阻抗是 `R_FIXED ∥ R_NTC`：常温下约 5 kΩ，
温度很低（NTC 阻值变大）时逼近 10 kΩ。这属于「高阻信号源」，采样时间必须给够。

数据手册的 ADC 特性里给了约束：

```
RAIN  <  k / ( fADC · CADC · ln(2^(N+2)) )  −  RADC
```

取 `k = 480`（周期数）、`fADC = 21 MHz`（见下）、`N = 12`，
算出来的上限在几百 kΩ 量级，本例的 10 kΩ 远远够用。
**480 是 F4 允许的最长档**，取它对高阻源最宽容，代价只是单次转换约 23 µs——
本工程一秒才采一次，可以忽略。

### 但 ADC 时钟必须先降下来

`k` 里的 `fADC` 由 `ClockPrescaler` 决定。F4 的 ADC 挂在 **PCLK2 = 84 MHz** 上，
而 CubeMX 的默认档是 `/2` = **42 MHz**，超过 F407 ADC 的 **36 MHz** 上限。

所以配方里显式写了 `ADC_CLOCK_SYNC_PCLK_DIV4` → 21 MHz。
依据：CubeMX 的 ADC IP 定义
`db/mcu/IP/ADC-aditf2_v1_1_Cube_Modes.xml` 里 `FadcConstraint` 的
`Max = 36000000`，且 `/2` 那一档自带
`Diagnostic="Prescaler divider not respecting Fadc Min Max Constraint"`。

⚠️ 这个参数**不会出现在 `.ioc` 里**（它等于 CubeMX 按本芯片约束解析出的默认值），
所以只能去生成的 `MX_ADC1_Init()` 里确认 `hadc1.Init.ClockPrescaler`。
详见 [`walkthrough.md`](walkthrough.md) 阶段 12.3。

## 软件 I2C 的电气前提

I2C 要求**任何一方只能把线拉低，不能主动拉高**，所以 SCL/SDA 必须是开漏输出，
高电平靠上拉电阻。本板的上拉来源有两处：

| 来源 | 阻值 | 作用 |
|---|---|---|
| OLED 模块板载 | 4.7 kΩ | **主要上拉**，实际决定上升沿 |
| STM32F4 内部上拉 | ~40 kΩ | 仅兜底：模块没插时总线不浮空，ACK 检测结果是稳定的「无应答」而不是随机值 |

> F4 的输出模式下内部上拉是**真的生效**的（F4 有独立的 PUPDR 寄存器）。
> 这一点和 STM32F1 相反——F1 的输出模式没有内部上拉，HAL 会静默忽略 `Pull` 字段。
> 所以从 F1 项目抄过来的注释里「内部上拉无效」那句话，在 F4 上**是错的**。

另外，RM0090 §8.3.10 写明：GPIO 配成输出时，施密特触发器仍然使能、
IDR 每个 AHB1 周期采样引脚电平。**位翻转 I2C 因此可以直接读回总线真实电平**
（`HAL_GPIO_ReadPin` 读的是 IDR），不必在输入/输出模式之间来回切换。
ACK 检测正是靠这一点。

## 生成物与手写代码的边界

| 路径 | 谁管 | 手写代码能否放 |
|---|---|---|
| `Core/`、`Drivers/`、`cmake/stm32cubemx/`、`startup_*.s`、`*.ld` | CubeMX | **只能放 `/* USER CODE BEGIN */ ~ END */` 内** |
| `CMakeLists.txt`（顶层）、`cmake/*.cmake` | 只生成一次 | 可以放心手写 |
| `app/` `bsp/` `driver/` `interface/` `module/` `docs/` `tools/` | 你 | 随便写 |
