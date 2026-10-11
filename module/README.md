# module/ — 业务逻辑

业务逻辑放这里，每个模块一个目录，一个模块一个职责。

```
module/
├── temperature/    温度采样与滤波（已实现：NTC 分压 → Beta 方程 → 毫摄氏度）
├── fan_control/    风扇转速换算与档位策略（已实现；PID 待做）
└── telemetry/      对外上报的运行数据组装 —— 计划中
```

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

- 类型：`typename_t`（如 `fan_state_t`）
- 函数：`module_action()`（如 `fan_control_set_duty()`、
  `temperature_from_adc_raw()`）
- 宏：`MODULE_DESCRIPTION`（如 `TEMPERATURE_NTC_B`）
- 文件内函数一律 `static`
- 跨模块取数据走函数（`fan_control_get_duty()`），不用 `extern` 全局变量

完整编码规范见 [`docs/architecture.md`](../docs/architecture.md)。
