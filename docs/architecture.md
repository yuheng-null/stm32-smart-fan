# 架构说明

## 分层与依赖方向

```
APP ──► MODULE ──► INTERFACE ──► BSP ──► PLATFORM
```

依赖**只能向下**，不允许反向、不允许成环。

| 层 | 目录 | 放什么 | 不许放什么 |
|---|---|---|---|
| APP | [`app/`](../app) | 启动编排、状态机、任务创建 | HAL 调用、寄存器访问、外设驱动 |
| MODULE | [`module/`](../module) | 业务逻辑（温控算法、风扇控制…） | HAL 调用、寄存器、CMSIS、RTOS 内部 API |
| INTERFACE | [`interface/`](../interface) | **只有契约**（函数指针表、错误码） | 任何实现、任何厂商头文件 |
| BSP | [`bsp/`](../bsp) | 把契约绑到具体引脚/外设上 | 业务判断 |
| PLATFORM | [`Core/`](../Core) + [`Drivers/`](../Drivers) | CubeMX 生成的 HAL 适配、CMSIS、启动文件、链接脚本 | 业务逻辑 |

「不许放什么」不是风格偏好：`app/` 里一旦出现 `HAL_GPIO_WritePin()`，
换芯片时就要改业务逻辑，而业务逻辑本该是唯一不用改的部分。

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
- **分层规则严格应用到手写代码上**——`app/`、`module/`、`interface/`、`bsp/` 这四个
  目录里，除 `bsp/` 外一律不许出现 HAL；`bsp/` 里也不许有业务判断。

代价是目录名不叫 `platform/`；实质要求（厂商代码与业务逻辑不混放）是满足的。

## CubeMX 管辖边界

| 路径 | 谁管 | 手写代码能否放 |
|---|---|---|
| `Core/`、`Drivers/`、`cmake/stm32cubemx/`、`startup_*.s`、`*.ld` | CubeMX | **只能放 `/* USER CODE BEGIN */ ~ END */` 块内**。块外改了，下次生成就没了 |
| `CMakeLists.txt`（顶层）、`cmake/stm32cubemx/CMakeLists.txt` | 只生成一次，之后不重写 | 可以放心手写 |
| `app/` `bsp/` `interface/` `module/` `docs/` `tools/` | 你 | 随便写 |

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

## 当前进度

已完成：时钟树（8 MHz HSE → 168 MHz）、SWD 调试口、USART1（PA9/PA10）、
分层骨架、串口链路验证。

`app/app_main.c` 目前是串口链路的冒烟验证（开机横幅 + 心跳 + 回显）。
下一步把温控风扇的状态机替换掉 `app_main_run()` 里的循环，业务逻辑放 `module/`。
