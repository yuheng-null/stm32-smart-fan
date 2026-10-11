# 从空文件夹到跑通：完整操作记录

本文按**实际开发顺序**记录这个工程是怎么建起来的，每一步都附**验收标准**。
命令可直接照抄（把 `<工程根>` 换成 `d:/c programing/embedded_learning/smart_fan`）。

工具链不在系统 PATH 上，每个新终端先导出（版本号用 `ls` 现查）：

```bash
export PATH="$HOME/AppData/Local/stm32cube/bundles/gnu-tools-for-stm32/14.3.1+st.2/bin:\
$HOME/AppData/Local/stm32cube/bundles/cmake/4.3.1+st.1/bin:\
$HOME/AppData/Local/stm32cube/bundles/ninja/1.13.2+st.1/bin:$PATH"
```

配置改动的下达用仓库自带的 [`tools/cubemx_apply.py`](../tools/cubemx_apply.py)：

```bash
python tools/cubemx_apply.py apply     <工程根> <配方文件>   # 按配方改配置
python tools/cubemx_apply.py roundtrip <工程根>             # 验证改动被原样接受
```

其余几步（建工程、生成代码、开中断）走 CubeMX GUI 或它的脚本模式
（`STM32CubeMX.exe -q <脚本文件>`，脚本每行一条命令、**最后一行必须是 `exit`**），
各步下面都会给出具体做法。

---

## 阶段 0：先摸清环境，别急着建工程

**先查，不要假设。** 本机工具全在 `stm32cube/bundles` 下，`command -v cmake` 会
告诉你「没装」——**那是误导**。

```bash
B="$HOME/AppData/Local/stm32cube/bundles"
ls "$B"                                    # 有哪些工具
ls "$B/gnu-tools-for-stm32"                # 编译器版本
ls -d "$HOME/STM32Cube/Repository"/*       # 有哪些固件包
ls "D:/STM32_HAL/STM32CubeMX/STM32CubeMX.exe"
```

验收：四项都非空。本机结果：编译器 `14.3.1+st.2`、cmake `4.3.1+st.1`、
ninja `1.13.2+st.1`、固件包有 F1 V1.8.7 与 **F4 V1.28.3**（F4 正是我们要的）。

---

## 阶段 1：明确输入 —— 先把「不能猜」的东西问清楚

两类信息**必须来自用户或数据手册**，不能自己编：

| 项 | 值 | 为什么不能猜 |
|---|---|---|
| MCU 型号 | STM32F407VET6 | —— |
| **晶振频率** | **8 MHz** | 决定 PLL 倍频与最终主频；填错串口波特率全错，而且编译烧录全都"成功" |

晶振频率这一条**必须问**：F407VET6 的板子有 8 MHz 的（多数核心板、正点原子探索者），
也有 25 MHz 的（野火霸道），还有 CubeMX 对 F4 的**默认值就是 25 MHz**。
填错的后果是主频按比例偏移（8→25 会超频到 525 MHz 跑飞），而没有任何一环会报错。

---

## 阶段 2：创建 `.ioc`

在 CubeMX GUI 里 **File → New Project** 选 `STM32F407VET6` 即可；
等价地，也可以用脚本模式（`-q`）一步建出来：

```text
load STM32F407VETx
project name smart_fan
project toolchain CMake
project path <父目录>
config saveas <工程根>/smart_fan.ioc
exit
```

产出：`<工程根>/smart_fan.ioc`。

> 型号写 `STM32F407VET6`（订货号），CubeMX 内部统一用 `STM32F407VETx`
> （「型号 + 封装通配」）表示同一颗芯片。**两者是同一颗芯片**，不是写错了。

**工程名必须等于文件夹名**：CubeMX 按「`project path` 的父目录 + `project name`」生成到
`<父目录>/<工程名>/`，只有两者相等时结果才正好落回同一个文件夹，从而让
**VSCode 工作区根 == CMake 工程目录**成立（ST 的全套工具都假设这一点）。

验收：`.ioc` 存在。此时它是 CubeMX 的默认状态——时钟还是 HSI 16 MHz。

---

## 阶段 3：配时钟（**必须分两遍**）

### 3.1 先查权威键集，不要推导

CubeMX 安装目录里带着**每一块官方开发板的 `.ioc`**，都是 CubeMX 自己生成的：

```bash
D:/STM32_HAL/STM32CubeMX/db/plugins/boardmanager/boards/C47_Discovery_STM32F407G-DISC1_STM32F407VG_Board_AllConfig.ioc
```

这就是官方 F407 板（同样 8 MHz 晶振 → 168 MHz）的权威参照。

### 3.2 第 1 遍：晶振模式 + 引脚 + HSE 频率

`recipes/r1-hse-usart-swd.txt` 见 [`cube-recipes/`](../cube-recipes)：

```text
set mode RCC "Crystal/Ceramic Resonator"
set pin PH0-OSC_IN RCC_OSC_IN
set pin PH1-OSC_OUT RCC_OSC_OUT
set ip parameters RCC HSE_VALUE 8000000
```

```bash
python tools/cubemx_apply.py apply "<工程根>" "cube-recipes/r1-hse-usart-swd.txt"
```

### 3.3 第 2 遍：重新加载，再配时钟树

```bash
python tools/cubemx_apply.py apply "<工程根>" "cube-recipes/r2-时钟树.txt"
```

### ⚠️ 为什么必须分两遍（本工程踩过的坑）

**`RCC.HSE_VALUE` 只在 CubeMX `config load` 那一刻被时钟模型读取。**

写在同一遍里的后果：命令全回 OK、`RCC.HSE_VALUE=8000000` 也确实落进了 `.ioc`
（还在白名单里）、`roundtrip` 报「原样保留」——**四层检查全绿**——
但模型仍按默认的 25 MHz 建树，算出 `SYSCLKFreq_VALUE=525000000`。
除了 CubeMX 弹的一个时钟警告窗，没有任何报错。

### 3.4 验收

```bash
grep -E "^RCC.SYSCLKFreq_VALUE=" "<工程根>/smart_fan.ioc"    # 必须是 168000000
python tools/cubemx_apply.py roundtrip "<工程根>"             # 必须报「原样保留」
```

且派生值应与官方板模板一致：

| 键 | 期望 |
|---|---|
| `RCC.VCOInputFreq_Value` | 1000000 |
| `RCC.VCOOutputFreq_Value` | 336000000 |
| `RCC.PLLCLKFreq_Value` | 168000000 |
| `RCC.PLLQCLKFreq_Value` | 48000000 |
| `RCC.APB1Freq_Value` / `RCC.APB2Freq_Value` | 42000000 / 84000000 |

> ⚠️ **`roundtrip` 的 `OK` 证明不了「取值合法」**。唯一会明说这件事的是 CubeMX 日志
> `~/.stm32cubemx/STM32CubeMX.log` 里的 `Parameter (X) has invalid value` /
> `not ready for code generation`。脚本每次跑完会自动把这类行打出来，**必须看**。

---

## 阶段 4：配 USART1 与 SWD

同样写在 r1 配方里：

```text
set pin PA13 SYS_JTMS-SWDIO
set pin PA14 SYS_JTCK-SWCLK

set mode USART1 "Asynchronous"
set pin PA9 USART1_TX
set pin PA10 USART1_RX
set ip parameters USART1 WordLength WORDLENGTH_8B
set ip parameters USART1 Parity PARITY_NONE
set ip parameters USART1 StopBits STOPBITS_1
```

### ⚠️ 取值不能按别的系列类推

**F4 的 USART 用短名**：`WORDLENGTH_8B` / `PARITY_NONE` / `STOPBITS_1`。
带 `UART_` 前缀的写法（`UART_WORDLENGTH_8B`）属于 **LPUART**，在 F4 的 USART 上非法。

权威出处（本机第一手材料）：

```
<CubeMX>/db/mcu/config/llConfig/USART-STM32F4xx_DefMapping.xml
<CubeMX>/db/mcu/compatibility.xml     # 里面有 (.*)_WORDLENGTH_8B → WORDLENGTH_8B 的迁移规则
```

**填错的后果特别阴险**：`set` 回 OK → 键落盘 → roundtrip 报「原样保留」→ 编译通过，
只有日志里一句 `Parameter (WordLength) has invalid value`；生成出来的
`MX_USART1_UART_Init()` **少三个字段**，而那几个宏恰好都等于 0，零初始化后碰巧还是 8N1
——错误被彻底掩盖，直到有人要改校验位才会炸。

> **默认值不落盘是正常的**：115200 是默认波特率，所以 `.ioc` 里**不会**出现
> `USART1.BaudRate`。这不是失败——去生成的代码里核对 `huart1.Init.BaudRate = 115200;`。

### `ProjectManager.MainLocation` 改一行

默认是 `Src`，会生成 `Src/` + `Inc/`，与 ST 官方布局不符。它没有任何 `set` 命令，
只能在 `.ioc` 里把这一行改掉（CubeMX 自己也是以这个键存的）：

```ini
ProjectManager.MainLocation=Core/Src
```

这是**全流程里唯一一处需要手工碰 `.ioc` 的地方**，理由是 CLI 确实没有对应命令，
而且它只影响输出目录布局、不涉及任何外设。改完跑 `roundtrip` 确认 CubeMX 原样保留。

---

## 阶段 5：生成代码，然后**去代码里找证据**

在 CubeMX GUI 里点 **GENERATE CODE**；等价地，用脚本模式：

```text
config load <工程根>/smart_fan.ioc
project name smart_fan
project toolchain CMake
project path <父目录>
project generate
exit
```

**生成前先把 `Core/` 下所有 `USER CODE` 块的内容快照下来**，生成后逐个核对是否幸存。
CubeMX 靠「读回这些块」来保留用户代码，而这个机制**偶尔会失效**——块被整段清空、
返回码是 0、一句报错都没有。核对过再往下走，别把「生成成功」当成「代码还在」。

### 验收（**别只看 `.ioc`**）

```bash
# 布局
ls Core/Inc Core/Src          # 必须是这个布局，不能有 Src/ + Inc/

# 时钟
sed -n '/void SystemClock_Config/,/^}/p' Core/Src/main.c
#   期望: HSEState=RCC_HSE_ON / PLLSource=RCC_PLLSOURCE_HSE
#         PLLM=8 PLLN=336 PLLP=RCC_PLLP_DIV2 PLLQ=7
#         APB1CLKDivider=RCC_HCLK_DIV4 APB2CLKDivider=RCC_HCLK_DIV2
#         FLASH_LATENCY_5

# 串口 —— 重点看那三行齐不齐
sed -n '/static void MX_USART1_UART_Init/,/^}/p' Core/Src/main.c
#   期望: BaudRate=115200 / WordLength / StopBits / Parity 一个不少

# HAL 模块开关
grep "HAL_UART_MODULE_ENABLED" Core/Inc/stm32f4xx_hal_conf.h
```

判据是**字段一个不少**——缺字段就是取值写错了（见上面那个坑）。

---

## 阶段 6：编译

```bash
cd "<工程根>"
cmake --preset Debug
cmake --build build/Debug
```

验收：看到 `Linking C executable smart_fan.elf` + 内存占用表。
报 `arm-none-eabi-gcc not found` **不是没装**，是 PATH 没导。

**每一处警告都要看**：本工程就是靠 `-Wformat-truncation` 抓到一个真 bug——
约 330 字节的中文横幅塞进 128 字节缓冲会被静默截断，而守卫函数遇截断返回错误，
导致横幅永远发不出去、程序原地死循环。

---

## 阶段 7：烧录

**先探测，再确认**——这会覆盖芯片上的现有固件：

```bash
B="$HOME/AppData/Local/stm32cube/bundles/programmer/2.23.0/bin"
"$B/STM32_Programmer_CLI.exe" -c port=SWD mode=UR
```

核对三项：`Device ID`（0x413 = F407）、`NVM size`（512 KB 对应 **VE**）、
`Voltage`（≈3.3 V）。**念给用户听，确认无误再烧。**

```bash
"$B/STM32_Programmer_CLI.exe" -c port=SWD mode=UR -w build/Debug/smart_fan.elf -v -rst
```

验收：`Download verified successfully`。`-v` 是校验，`-rst` 是烧完复位运行。

---

## 阶段 8：验证 —— 硬件行为说话

```bash
python tools/capture_serial.py                  # 复位 + 抓横幅与心跳
python tools/capture_serial.py --echo "PING"    # 验证 RX 回显
```

脚本会打印每条消息的**到达时刻**，并算心跳间隔。

### 心跳间隔是对时钟的独立验证

固件按 1000 tick 排心跳。若实际晶振与 `HSE_VALUE` 不符，SysTick 重装值按错误主频算，
真实间隔就按比例偏移：

| 实际情况 | 心跳真实间隔 |
|---|---|
| 8 MHz HSE → 168 MHz（正确） | **1.000 s** |
| 晶振实为 25 MHz 却按 8 MHz 配（HCLK 变 525 MHz） | 约 **0.32 s** |

「命令返回 OK」和「生成代码看着对」都证明不了实际主频，**这条能**。

### 出问题时：用调试器读寄存器，别猜接线

RX 不通时，用 ST-Link + GDB 直接读芯片内部状态，一次把「接线问题」和「固件问题」分开：

| 寄存器 | 地址 | 看什么 |
|---|---|---|
| `GPIOA MODER` | `0x40020000` | PA9/PA10 是否 = `0b10`（复用模式） |
| `GPIOA AFR[1]` | `0x40020024` | PA9/PA10 是否 = `7`（AF7 = USART1） |
| `GPIOA PUPDR` | `0x4002000C` | 上下拉配置 |
| `GPIOA IDR` | `0x40020010` | **bit9 = PA9 电平，bit10 = PA10 电平**（UART 空闲必须是高） |
| `USART1 SR` | `0x40011000` | RXNE / ORE / FE / NE 标志 |
| `USART1 BRR` | `0x40011008` | 波特率分频（84 MHz ÷ (16×115200) = 45.5625 → `0x2D9`） |
| `USART1 CR1` | `0x4001100C` | UE(bit13) / TE(bit3) / RE(bit2) |

配套的判定手法——**用内部上拉/下拉探外部有没有在驱动这根线**：

- 给引脚挂**内部上拉**（PUPDR = `0b01`）后仍读到 **0** → **有外部低阻通路把它拉低**（接线短地）；
- 挂上拉后读到 **1** → 引脚悬空或外部驱动高。
- **必须做对照组**：同时给一个肯定悬空的引脚挂上拉。若对照组能变 1，说明上拉机制有效，
  结论才成立。（本工程第一次做这个测试时忘了对照组，结论差点下过头。）

---

## 阶段 9：搭分层骨架

目录（详见 [`architecture.md`](architecture.md)）：

```
interface/   纯契约（uart_if.h、error.h）—— 无实现、无厂商头文件
bsp/         把契约绑到具体外设与引脚
app/         启动编排、状态机
module/      业务逻辑
docs/ tools/ cube-recipes/
```

挂进顶层 `CMakeLists.txt`（该文件**只在首次生成时产出，之后不重写**，可放心手写）：

```cmake
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    app/app_main.c
    bsp/bsp_system.c
    bsp/bsp_uart.c
)
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
```

手里要写在生成文件里的东西（`main()` 里的调用）只能放 `USER CODE` 块，
`bsp_uart.c` 要用的 `huart1` 句柄在 `main.h` 的 `USER CODE BEGIN EFP` 里补 `extern`。

**验收：跑一次 `regenerate`，确认 USER CODE 全部幸存。** 本工程实测 14 个非空块全在。

---

## 阶段 10：收数据改成中断 + 环形缓冲

USART1 的 NVIC 使能是 CubeMX **CLI 唯一没有命令**的项。两种做法，任选一种：

- **GUI**：Pinout & Configuration → System Core → NVIC → 勾上 `USART1 global interrupt`；
- **改 `.ioc`**：加上这一行（`.ioc` 里 `:` 要写成 `\:`）。前两个数字是抢占/子优先级：

  ```ini
  NVIC.USART1_IRQn=true\:2\:0\:false\:false\:true\:true\:true\:true
  ```

优先级为什么取 **2**：按「控制相关的优先级必须高于通信」的原则，将来温控环的定时器
中断会占 0/1，串口排在它后面。串口晚一点响应没有代价——115200 下一个字节有 87 µs 余量；
反过来，若让串口打断控制环，控制周期就会抖。

生成后**必须去代码里确认**：

```bash
sed -n '/void USART1_IRQHandler/,/^}/p' Core/Src/stm32f4xx_it.c   # 应调 HAL_UART_IRQHandler
grep -A2 "USART1_IRQn" Core/Src/stm32f4xx_hal_msp.c               # 应有 SetPriority + EnableIRQ
```

中断接收的两条铁律（都是读 HAL 源码确认的，不是推测）：

1. `UART_Receive_IT()` 收满后会把 **RXNE / PE / ERR 三个中断全关掉**、`RxState` 置回
   READY，**然后才调回调** → 回调里**必须**重新 `HAL_UART_Receive_IT()`，
   否则只收得到一个字节，且不报错。
2. 溢出（ORE）被 HAL 当作**阻塞性错误**：先 `UART_EndRxTransfer()` 中止整个接收，
   **然后才**调 `HAL_UART_ErrorCallback` → 错误回调里**必须**清标志 + 重新武装，
   否则接收永久停在坏状态，之后只能发不能收。

契约也要跟着改：`read()` 定义成**非阻塞、返回实收字节数**，而不是「阻塞等 N 字节」。

### 效果对比（同一块板、同一根线）

| 连续发送 | 轮询版 | 中断 + 环形缓冲版 |
|---|---|---|
| 20 字节背靠背 | 19/20 **丢 1** | 20/20 |
| 64 / 200 / 512 字节背靠背 | —— | **全部丢 0** |

---

## 阶段 11：VSCode 集成

三份配置写在**工程根**（工作区根必须 == CMake 工程根，否则 ST 的工具链
会以「多工程警告 / 调试 Device not found / Build Analyzer 报错」三种面目失败）：

- `.clangd` → `CompilationDatabase: build/Debug`
  （CMakePresets 把库放在这，不在 clangd 默认找的两处；不加则满屏红波浪线）
- `.vscode/settings.json` → `cmake.useCMakePresets: always`、关掉 C/C++ 的 IntelliSense
- `.vscode/launch.json` → `cortex-debug` + `servertype: "stlink"`
  （**不用** ST 官方那个：它失败时不写任何日志，只能猜）、
  `device` 用 **CMSIS 名** `STM32F407VE`（不是 `STM32F407VETx`）

验收（可脱离 VSCode 单独验）：

```bash
"$B/st-arm-clangd/21.1.0+st.2/bin/starm-clangd.exe" --check=app/app_main.c
# 期望: All checks completed, 0 errors
```

---

## 阶段 12：加 OLED / NTC / 按键三路驱动

这一阶段第一次用到 CubeMX 脚本里**针对 GPIO 与 ADC** 的命令，新踩的坑记在这里。

### 12.1 引脚的电气属性与标签要用 `set gpio parameters`

`set pin` 只能把引脚配成 `GPIO_Output` / `GPIO_Input` / `GPIO_Analog`，
**管不到开漏、上下拉、标签**，这些要用另一个命令：

```text
set pin PB6 GPIO_Output
set gpio parameters "PB6" "GPIO_ModeDefaultOutputPP" "GPIO_MODE_OUTPUT_OD"
set gpio parameters "PB6" "GPIO_PuPd" "GPIO_PULLUP"
set gpio parameters "PB6" "GPIO_Label" "OLED_SCL"
```

⚠️ 输出脚的「模式参数」名字是 **`GPIO_ModeDefaultOutputPP`**，输入脚才叫
`GPIO_Mode`；而且**输入脚根本不需要**写模式参数（信号名 `GPIO_Input`
已经表达了）。依据是 CubeMX 自己的 IP 定义
`db/mcu/IP/GPIO-STM32F417_gpio_v1_0_Modes.xml`，以及既有工程 `.ioc` 里
`GPIOParameters` 的**实际键名**。写错名字不会明着报错，只会让这次 `set` 落空。

标签写 `OLED_SCL`，生成的 `main.h` 里就是 `OLED_SCL_Pin` / `OLED_SCL_GPIO_Port`。

### 12.2 ADC 的时钟分频必须显式确认，默认档会超限

F4 的 ADC 挂在 PCLK2 = 84 MHz 上，而 `ClockPrescaler` 的默认档是 `/2` = **42 MHz**，
超过 F407 ADC 的 **36 MHz** 上限。

依据：`db/mcu/IP/ADC-aditf2_v1_1_Cube_Modes.xml`（F407VET6 用的是
`aditf2_v1_1_Cube`，见 `db/mcu/STM32F407V(E-G)Tx.xml`），其中 `FadcConstraint`
的 `Max="36000000"`，且 `/2` 那一档自带
`Diagnostic="Prescaler divider not respecting Fadc Min Max Constraint"`。

写 `ADC_CLOCK_SYNC_PCLK_DIV4`（21 MHz）即可。

### 12.3 「参数没写进 `.ioc`」≠「命令没生效」

这次 `ClockPrescaler` 与 `ScanConvMode` 都**没**出现在 `.ioc` 里，但生成的
`MX_ADC1_Init()` 里两者都是我们要的值（`DIV4` / `DISABLE`）。

原因：CubeMX 只序列化**与默认值不同**的参数，而这两个的「默认值」是它按本芯片
约束解析出来的，不是 IP 定义 XML 上那个字面默认值。

**判据永远是生成的代码，不是 `.ioc` 的键。** `.ioc` 缺一个键有两种完全不同的
原因——命令落空、或值恰好等于默认——光看 `.ioc` 分不出来。

### 12.4 `apply` 不生成代码

`tools/cubemx_apply.py apply` 只把配方下达给 `.ioc`。要出代码得**再跑一次生成**
（GUI 点 GENERATE CODE，或 `project generate` 脚本，`project path` 给**父目录**）。
忘了这一步的症状：`main.h` 里没有新引脚的宏、`main.c` 里没有 `MX_ADC1_Init()`。

生成前先快照 `Core/` 下所有 USER CODE 块，生成后逐块 diff 核对。

### 12.5 STM32F4 没有 ADC 校准

F1/F3 的 ADC 有硬件自校准（`HAL_ADCEx_Calibration_Start`），漏掉会有几十 LSB
的固定偏差。**F4 没有**：`ADC_CR2` 里没有 ADCAL 位，F4 的 HAL 库里也不存在这个
函数（已在本机 `Drivers/STM32F4xx_HAL_Driver/` 全库检索确认）。
从 F1 项目移植代码时照抄那一行会直接编译不过——这是好事，编译期就挡住了。

### 12.6 验收

```bash
# 1) 宏与初始化函数是否真的生成了
grep -n "OLED_SCL_Pin\|KEY1_Pin" Core/Inc/main.h
sed -n '/static void MX_ADC1_Init/,/^}/p' Core/Src/main.c
#   期望: ClockPrescaler=DIV4 / ScanConvMode=DISABLE / Channel=ADC_CHANNEL_0
#         SamplingTime=ADC_SAMPLETIME_480CYCLES / Rank=1

# 2) GPIO 的电气属性对不对（开漏 + 上拉、输入 + 上拉）
sed -n '/static void MX_GPIO_Init/,/^}/p' Core/Src/main.c
#   期望: OLED 那两个脚 GPIO_MODE_OUTPUT_OD + GPIO_PULLUP
#         KEY 那两个脚 GPIO_MODE_INPUT + GPIO_PULLUP

# 3) PA0 是不是配成了模拟输入
sed -n '/void HAL_ADC_MspInit/,/^}/p' Core/Src/stm32f4xx_hal_msp.c
#   期望: GPIO_MODE_ANALOG + GPIO_NOPULL

# 4) HAL 模块开关与源文件清单
grep "HAL_ADC_MODULE_ENABLED" Core/Inc/stm32f4xx_hal_conf.h
grep "hal_adc" cmake/stm32cubemx/CMakeLists.txt
```

---

## 收尾自检清单

- [ ] `roundtrip` 报「原样保留」，且**没有白名单缺口**
- [ ] **没有任何 `Parameter (X) has invalid value` / `not ready for code generation`**
      —— 这是唯一能证明「取值合法」的信号
- [ ] `apply` 之后**另外跑过一遍代码生成**（`apply` 自己不生成代码）
- [ ] 生成后把 `Core/` 下的 USER CODE 块**逐块 diff** 过，没有丢失或被改写
- [ ] `.ioc` 里缺的键，去**生成的代码**里确认过取值（缺键≠没生效，见阶段 12.3）
- [ ] `SYSCLKFreq_VALUE` 等于目标主频
- [ ] 去生成的 `.c` 里**逐项找到代码**，外设初始化函数**字段一个不少**
- [ ] `cmake --build` 通过，**零警告**
- [ ] 烧录探测的 `Device ID` / `NVM size` 与型号一致，且**烧录前经过用户确认**
- [ ] `Download verified successfully`
- [ ] **硬件行为由用户或实测数据确认** —— 别把「命令返回 0」当成「功能正常」
