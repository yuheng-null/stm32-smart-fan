# 架构说明

## 分层与依赖方向

与其画一张会看错的方框图，不如把**「谁可以调用谁」**直接列出来——
依赖是单向的，反过来一律不允许：

```
调用方 ──────► 被调用方

APP            ──►  MODULE、BSP、INTERFACE
MODULE         ──►  INTERFACE
BSP            ──►  DRIVER/device、DRIVER/bus、INTERFACE、PLATFORM
DRIVER/device  ──►  INTERFACE
DRIVER/bus     ──►  INTERFACE、PLATFORM
```

几条关键性质：

- **`INTERFACE` 是被依赖得最多、自己谁也不依赖的一层。** 它只有契约
  （`error.h`、`uart_if.h`、`i2c_if.h`），没有实现、没有厂商头文件。
- **`MODULE` 和 `DRIVER/device` 都只依赖 `INTERFACE`**，所以它们两个都不认识
  STM32。业务逻辑和器件驱动换芯片时都不用改。
- **`DRIVER/bus` 是唯一直接贴着 HAL 的驱动层**，它的职责就是**实现**
  `INTERFACE` 里的总线契约（比如用 GPIO 位翻转实现 `i2c_if_t`）。
- **`BSP` 是唯一同时认识「契约」和「本板引脚」的手写层**，它把驱动绑到具体引脚上：
  「这块板子上，屏挂在哪条总线、按键接在哪两个脚」。
- 画成「上中下」反而容易误解——`APP` 可以直接调 `BSP`（`app_main.c` 就是这么
  取 UART 契约的），这不是越层，因为 `BSP` 就在 `APP` 的下方。

依赖**只能向下**，不允许反向、不允许成环。

### `module/` 内部：允许一条单向的「组合」依赖

上面那张表只说了 `MODULE → INTERFACE`，没说 module 之间能不能互相依赖。
本工程的选择是：**允许，但只允许一条，且必须是单向的**。

```
module/ 内部（叶子只依赖 interface/ 与 common/）
    fan_control  fan_curve  pi_control  fan_fsm  console  telemetry  display_view  ir_remote
         ▲             ▲            ▲
         │             │            │
         └─────── thermal_control ──┘        ← 唯一的「组合」模块
```

- **叶子**：`fan_control`、`fan_curve`、`pi_control`、`fan_fsm`、`console`、
  `telemetry`、`display_view`、`ir_remote` —— 只依赖 `interface/` 与 `module/common/`，
  互不认识。
- **组合**：`thermal_control` 可以依赖 `fan_curve`、`pi_control`、`fan_control`
  —— 它本来就是「把算法拼起来」的那一层。
- **禁止**反向与成环。`fan_curve` 不能去 include `thermal_control`。

这样做的收益很直接：除了 `thermal_control` 这一处，
**每个模块都能单独编译、单独测试**，测试时不必先把整条链搭起来。
`fan_fsm` 尤其如此——它连 `thermal_control` 都不认识，app 把算好的占空比
当成一个整数喂给它，于是「喂一串按键事件，看模式怎么变」的测试朴素得像查表。

`module/common/control_types.h` 是**只有类型、没有代码**的头文件，谁都可以
include 它，因此不会成为环的载体。

| 层 | 目录 | 放什么 | 不许放什么 |
|---|---|---|---|
| APP | [`app/`](../app) | 启动编排、状态机、任务创建 | HAL 调用、寄存器访问、外设驱动 |
| MODULE | [`module/`](../module) | 业务逻辑（温控算法、风扇控制…） | HAL 调用、寄存器、CMSIS、RTOS 内部 API |
| INTERFACE | [`interface/`](../interface) | **只有契约**（函数指针表、错误码） | 任何实现、任何厂商头文件 |
| DRIVER/device | [`driver/device/`](../driver/device) | 具体器件的驱动（SSD1306…）：寄存器含义、显存、字符绘制 | HAL/CMSIS、任何 STM32 符号、引脚号 |
| DRIVER/bus | [`driver/bus/`](../driver/bus) | 总线实现（软件 I2C…） | 业务判断、具体器件的协议 |
| BSP | [`bsp/`](../bsp) | 把契约与驱动绑到具体引脚/外设上 | 业务判断 |
| PLATFORM | [`Core/`](../Core) + [`Drivers/`](../Drivers) | CubeMX 生成的 HAL 适配、CMSIS、启动文件、链接脚本 | 业务逻辑 |

「不许放什么」不是风格偏好：`app/` 里一旦出现 `HAL_GPIO_WritePin()`，
换芯片时就要改业务逻辑，而业务逻辑本该是唯一不用改的部分。
同理，`driver/device/ssd1306.c` 里若出现 `HAL_I2C_*`，它就绑死在了 STM32 上——
现在它只认 `interface/i2c_if.h`，换到 GD32/ESP32 只需换一份总线实现。

### 契约与实现分开带来的一个直接好处

`ssd1306.c` 的全部输入是 `i2c_if_t`（三个函数指针）。在 PC 上给一份
「把收到的字节打印出来」的假实现，就能不接硬件测显存组织、字符绘制、
边界处理对不对——这类 bug 在硬件上调试一次的成本远高于写一个假实现。

## ⚠️ 一处与通用规范的偏离：`platform/` 由 `Core/` + `Drivers/` 承担

通用嵌入式架构规范要求把厂商代码放进 `platform/<型号>/`，且**禁止**在仓库根出现
`Core/`、`Drivers/`。**本工程刻意不这么做**，理由：

CubeMX 的代码生成路径是它自己定的——`<工程根>/Core`、`<工程根>/Drivers`，
CLI 和 GUI 都没有「生成到 `platform/stm32f407/` 下」这个选项。要让文件待在
`platform/` 里，就得**每次 CubeMX 重新生成之后手工搬一次**，而 CubeMX 重新生成是
**静默**的：不报错、不警告，直接写回原处。这种「手写与生成代码争夺同一份事实」
的局，最后一定是两边漂移。

所以本工程的取舍是：

- **厂商代码留在 `Core/` + `Drivers/`**，由 CubeMX 全权掌管，零人工搬运；
- **分层规则严格应用到手写代码上**——`app/`、`module/`、`interface/`、
  `driver/device/` 一律不许出现 HAL；允许贴 HAL 的只有两个地方：
  `bsp/` 和 `driver/bus/`，而且这两处也不许有业务判断。

代价是目录名不叫 `platform/`；实质要求（厂商代码与业务逻辑不混放）是满足的。

## CubeMX 管辖边界

| 路径 | 谁管 | 手写代码能否放 |
|---|---|---|
| `Core/`、`Drivers/`、`cmake/stm32cubemx/`、`startup_*.s`、`*.ld` | CubeMX | **只能放 `/* USER CODE BEGIN */ ~ END */` 块内**。块外改了，下次生成就没了 |
| `CMakeLists.txt`（顶层）、`cmake/` 下的构建脚本（`*.cmake` 工具链文件与 `cmake/stm32cubemx/CMakeLists.txt`） | 只生成一次，之后不重写 | 可以放心手写 |
| `app/` `bsp/` `driver/` `interface/` `module/` `docs/` `tools/` | 你 | 随便写 |

判断某文件会不会被覆盖：看工程根的 `.mxproject` 里的 `[PreviousGenFiles]` 段，
不在其中的文件不会被重新生成。

### `cmake/stm32cubemx/CMakeLists.txt` 的源文件清单要手工补

新增 HAL 模块时，CubeMX **不会**更新这个文件的源文件清单。漏了的症状是链接期
`undefined reference to 'HAL_xxx_Init'`。把对应的 `.c` 加进 `STM32_Drivers_Src`。

（本工程配 USART1 时 CubeMX 自动加上了 `stm32f4xx_hal_uart.c`，但这条不可依赖。）

## 修改配置的唯一入口：CubeMX

**外设、引脚、时钟树一律通过 CubeMX 改，绝不手改 `.ioc` 来加外设。**

`.ioc` 是 CubeMX 内部状态的序列化，`Mcu.IPn` 不是开关，而是「该外设已激活」这个
事实的**记录**。手写这个记录，CubeMX 重建不出对应状态就整块静默丢弃——连同该外设
的全部参数。

本工程的配置下达方式是**配方文件 + CubeMX 脚本模式**。仓库自带下达工具
[`tools/cubemx_apply.py`](../tools/cubemx_apply.py)，它只是把配方喂给
CubeMX 自己的 `-q <脚本>` 接口，没有自己解析 `.ioc`：

```bash
python tools/cubemx_apply.py apply . cube-recipes/r1-hse-usart-swd.txt
python tools/cubemx_apply.py apply . cube-recipes/r2-时钟树.txt
```

### ⚠️ 为什么时钟树必须跑两遍

见 [`cube-recipes/r2-时钟树.txt`](../cube-recipes/r2-时钟树.txt) 顶部的完整说明。
一句话：**`RCC.HSE_VALUE` 只在 `config load` 那一刻被时钟模型读取**。同一次会话里
「先写 HSE_VALUE、再配时钟」，命令全回 OK、键也落盘、roundtrip 还报「原样保留」，
但模型仍按 25 MHz 建树，算出 525 MHz 的严重超频——唯一的外部信号是 CubeMX 弹的
时钟警告窗。

## 验证纪律

这套工具链的典型失败模式是**每一步都返回成功，但产物是错的**。所以：

```
参数被接受  ≠  取值合法  ≠  代码被生成  ≠  硬件行为正确
```

四件事各自独立，且失败时都不报错。每次改完配置，四条都要单独验：

1. `python tools/cubemx_apply.py roundtrip .` —— 报 `OK: 写的参数全部被 CubeMX 原样保留了`
2. 查 CubeMX 日志 `~/.stm32cubemx/STM32CubeMX.log` 里有没有
   `Parameter (X) has invalid value` / `not ready for code generation`
   （**这是唯一能证明「取值合法」的信号**，roundtrip 比不出来）
3. 去生成的 `.c` 里逐项找到对应代码（`MX_xxx_Init()` 的**字段一个不少**）
4. 让硬件行为说话——[`tools/capture_serial.py`](../tools/capture_serial.py)

第 4 条不能省，也不能用前三条替代。本工程的时钟树就是靠心跳间隔实测出来的：
固件按 1000 tick 排心跳，若实际晶振与 `HSE_VALUE` 不符，真实间隔就会偏离 1 秒。

## 平台事实：STM32F4 与 STM32F1 的差异（都查过出处，不是类推）

这几条在移植 F1 的代码风格过来时最容易想当然，单独记下来：

| 事实 | STM32F1 | **STM32F4（本工程）** | 出处 |
|---|---|---|---|
| ADC 硬件自校准 | 有（`ADC_CR2.CAL`），HAL 为 `HAL_ADCEx_Calibration_Start()`；漏掉会有几十 LSB 固定偏差 | **没有**。`stm32f407xx.h` 里 `ADC_CR2` 只有 ADON/CONT/DMA/DDS/EOCS/ALIGN/…，**无 CAL 位**；`Drivers/STM32F4xx_HAL_Driver/` 全库搜不到 `HAL_ADCEx_Calibration_Start` | 本机 CMSIS 头 + HAL 源码；ST 社区亦确认「F4 系列 ADC 没有内建校准功能」 |
| 开漏输出时能否读回引脚电平 | 能（输出模式下输入驱动器仍使能） | **能**。RM0090 §8.3.10 Output configuration：输出模式下施密特触发器仍使能，IDR 每个 AHB1 周期采样引脚，「a read access to the input data register gets the I/O state」 | RM0090 §8.3.10 |
| 输出模式下的内部上拉 | **无效**（F1 无 PUPDR，上下拉只存在于输入模式，HAL 静默忽略 `Pull` 字段） | **有效**（F4 有独立的 PUPDR）。但 ~40 kΩ 太弱，总线仍靠从机器件板载的 4.7 kΩ | RM0090 §8.3.2 |
| `HAL_ADC_PollForConversion` | 扫描模式 / 序列长度 >1 时**不查标志**，只空转最坏时间 | **无条件死等 EOC 标志**，单通道轮询可靠 | 本机 `stm32f4xx_hal_adc.c` |

> 反过来说：移植 F1 代码到 F4 时，「照抄那条 ADC 校准」会直接编译不过
> （符号不存在）——这是好事，编译期就挡住了；而「照抄 F1 关于内部上拉
> 不生效的注释」则不会报错，只会写下一句错的硬件描述。上表就是为了防后者。

## 当前进度

**平台**：时钟树（8 MHz HSE → 168 MHz）、SWD、USART1（PA9/PA10，中断接收 + 环形缓冲）、
分层骨架；外设：OLED（软 I2C）、NTC 测温（ADC1）、按键、风扇 PWM + FG 测速、
继电器、有源蜂鸣器 —— 全部实测通过。

**业务层**：温控级联（分段曲线 + 迟滞 → 内环 PI → 占空比）、运行模式状态机、
传感器降级与失效安全、串口命令协议与遥测帧、OLED 视图。
`app/app_main.c` 是一个**时间触发**的协作式调度器（每任务各自周期），
业务逻辑全部在 `module/` 下且为**纯计算**，用宿主机单元测试覆盖
（`tests/`，9 个测试目标，见 [README](../README.md) 的「测试」一节）。

**已上板实测**：手动调速、闭环收敛、升温响应与迟滞换挡、遥测间隔
（→ 实测主频 168.2 MHz）。内环 PI 的增益是**基于板上阶跃辨识整定**的
（τ ≈ 1.4 s），第一版按假定值整定曾导致持续振荡——完整经过见
[`control.md`](control.md) §4。

**标定**：NTC 的温度刻度与 rpm 的绝对倍率按器件标称值入场，都支持现场标定。
两者只影响显示数字的**物理刻度**，不影响控制正确性
（闭环对倍率误差免疫，见 [`control.md`](control.md)）。标定步骤写在
[`control.md`](control.md) 的「标定」一节。

**验证纪律**：改完控制逻辑后按 README 的「上板验收清单」过一遍 ——
编译通过、单测全绿、CI 绿都证明不了硬件行为，那是唯一能证明的一层。

### 一个分层的实例：为什么「占空比 → CCR」不在 module 里

`fan_control_set_duty_percent()` 收的是百分比，而定时器要的是比较值 CCR。这段换算
看起来像业务，其实不是：它只在本颗定时器、这个 `ARR` 下才有意义，是**硬件映射**，
所以留在 `bsp/bsp_fan.c` 里。

放在 `module/` 里技术上也能跑，但那会让 `bsp/` 反向去 `#include "module/..."`，
而 `BSP → MODULE` 是这张依赖表里明确不允许的方向。同理，`bsp_fan.c` 里宁可
自己定义一份 `BSP_FAN_DUTY_MAX_PERCENT`，也不去引 `module/fan_control` 的同名宏。
