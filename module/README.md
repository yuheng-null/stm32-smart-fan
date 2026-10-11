# module/ — 业务逻辑

业务逻辑放这里，每个模块一个目录，一个模块一个职责。

```
module/
├── common/           共享值类型（采样快照、模式、报警位）—— 只有头文件、无代码
├── temperature/      NTC 分压 → Beta 方程 → 毫摄氏度
├── fan_control/      FG 周期 → 转速、档位步进、转速↔占空比经验模型
├── fan_curve/        温度 → 目标转速（分段线性插值 + 迟滞）
├── pi_control/       通用 PI（条件积分抗饱和 / 限幅 / 限速率，无 D 项）
├── thermal_control/  级联控制器 = fan_curve + pi_control（唯一的「组合」模块）
├── fan_fsm/          运行模式状态机（纯策略，不含数值算法）
├── console/          串口行命令解析（字节流 → 命令结构）
├── telemetry/        遥测帧格式化（状态 → 一行文本）
└── display_view/     OLED 视图模型（状态 → 几行文本，约束每行宽度）
```

## `module/` 内部的依赖规则

上面那张表在 `docs/architecture.md` 里只说了 `MODULE → INTERFACE`。
本工程对 module **之间**的依赖有一条明确规则：

- **叶子模块**（`fan_control`、`fan_curve`、`pi_control`、`fan_fsm`、`console`、
  `telemetry`、`display_view`）只依赖 `interface/` 与 `common/`，**互不认识**。
- **组合模块** `thermal_control` 可以依赖 `fan_curve`、`pi_control`、`fan_control`
  —— 它本来就是「把算法拼起来」的那一层。
- **禁止反向、禁止成环。**

于是除了 `thermal_control` 一处，**每个模块都能单独编译、单独测试**。
`fan_fsm` 尤其如此：它连 `thermal_control` 都不认识，app 把算好的占空比当成一个
整数喂进去，于是「喂一串按键事件、看模式怎么变」的测试朴素得像查表。

## 当前实现：`fan_control/`

```c
error_t fan_control_rpm_from_period_us(uint32_t period_us, uint32_t *rpm);
uint8_t fan_control_duty_step(uint8_t duty, fan_duty_dir_t dir);
```

和 `temperature/` 一样是**纯计算**：输入是「FG 相邻两沿之间数了多少个 1 µs 计数」
和「当前占空比」，输出是转速和下一档占空比。不需要板子、不需要风扇就能单测。

⚠️ 转速换算依赖两个假设：**每转 2 个脉冲**（Intel 规范值）和**每个边沿只被捕获
一次**。实测线性度很好，但绝对值尚未与风扇铭牌对过；若差固定倍数，改
`FAN_CONTROL_PULSES_PER_REV`，或去查 FG 边沿是不是有振铃被重复捕获。

> 这里**没有**「占空比百分比 → 定时器比较值」那段换算：那是硬件映射，
> 留在 `bsp/bsp_fan.c` 里。原因是分层——`BSP → MODULE` 是反向依赖，
> 两边互不认识，硬要共用会让 `bsp/` 挂上业务层的东西。

## 当前实现：`temperature/`

```c
uint32_t temperature_resistance_from_raw(uint16_t raw);              /* 调试/校准用 */
error_t  temperature_from_adc_raw(uint16_t raw, int32_t *milli_c);   /* 主接口 */
```

它**只做换算**，不做采样：输入是一个 12 位 ADC 计数，输出是毫摄氏度。
采样动作在 [`bsp/bsp_adc.c`](../bsp/bsp_adc.c) 里。

这样切开的好处是这部分**完全可以在 PC 上单测**——给几个计数、核对温度，
不需要板子，也不需要 NTC。

⚠️ **参数是假定值，未校准**：`R25` / `B` / 分压电阻三个常数按这类模块的常见配置
取（10 kΩ、B=3950），依据的是模块型号而不是实测。所以在校准之前，
显示的温度**只看趋势、不看绝对值**。校准方法和两点的联立方程写在
[`temperature/temperature.h`](temperature/temperature.h) 顶部。

## 控制相关的模块：先看 `docs/control.md`

`fan_curve` / `pi_control` / `thermal_control` / `fan_fsm` 这四个模块合起来
就是温控系统本身。**它们的算法、参数来历、状态迁移图、以及哪些数字还没实测，
都写在 [`docs/control.md`](../docs/control.md)**，本文不重复。

一句话版：温度经**分段曲线（带迟滞）**得到目标转速，**内环 PI** 再用 FG 实测
转速把占空比调到那个目标上；`fan_fsm` 管运行模式与降级，但它**不认识**
`thermal_control`——app 把算好的占空比当成一个整数喂给它。

## 硬性约束

- **不许出现 HAL 调用**（`HAL_GPIO_*`、`HAL_UART_*`、`HAL_ADC_*` …），
  也不许直接访问寄存器或 CMSIS。
- 要用硬件能力，**只通过 `interface/` 的契约**。模块不该知道用的是
  USART1 还是别的串口、温度传感器接在哪个引脚、主控是哪颗芯片。
- 依赖方向只能向下：`MODULE → INTERFACE`。反向依赖与循环依赖都不允许。
- 不调用 `malloc`/`free`，不递归——嵌入式上栈和堆都是稀缺资源。

理由不是洁癖：业务逻辑是**唯一在换芯片时不用改的部分**。
一旦它里面出现 `HAL_ADC_GetValue()`，这个性质就没了。

## 命名

- 类型：`typename_t`（如 `fan_curve_t`、`pi_t`）
- 函数：`module_action()`（如 `fan_curve_eval()`、
  `temperature_from_adc_raw()`）
- 宏：`MODULE_DESCRIPTION`（如 `TEMPERATURE_NTC_B`、`FAN_CURVE_...`）
- 文件内函数一律 `static`
- 跨模块取数据走函数（`thermal_control_get_status()`），不用 `extern` 全局变量
- 实例状态放在结构体里由调用者持有（`fan_curve_t`、`pi_t`…），
  **不用文件级可变全局**——这样同一个模块可以并存多个实例，测试也不必复位全局

## 加一个新模块时

1. `module/<名字>/<名字>.c` + `.h`，头文件只 include `interface/` 与 `common/`。
2. 在 `tests/CMakeLists.txt` 的 `SMART_FAN_MODULE_SOURCES` 里加上 `.c`，
   在顶层 `CMakeLists.txt` 的 `target_sources` 里也加上。
3. 写一个 `tests/test_<名字>.c` 并在 `tests/CMakeLists.txt` 里注册——
   **能被单测是这个分层存在的主要理由，别只写实现不写测试**。
4. 如果新模块依赖别的 module（应该很少见），回到本文顶部的依赖规则确认
   它是不是「组合」模块，并在 `docs/architecture.md` 里补一笔。

完整编码规范见 [`docs/architecture.md`](../docs/architecture.md)。
