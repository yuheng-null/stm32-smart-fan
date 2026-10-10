# module/ — 业务逻辑

温控风扇的业务逻辑放这里，每个模块一个目录，一个模块一个职责。

计划中的模块：

```
module/
├── temperature/    温度采样与滤波（NTC / DS18B20 / 内部传感器）
├── fan_control/    风扇调速策略（开关控制 / PID）
└── telemetry/      对外上报的运行数据组装
```

## 硬性约束

- **不许出现 HAL 调用**（`HAL_GPIO_*`、`HAL_UART_*`、`HAL_ADC_*` …），
  也不许直接访问寄存器或 CMSIS。
- 要用硬件能力，**只通过 `interface/` 的契约**。模块不该知道用的是
  USART1 还是别的串口、温度传感器接在哪个引脚、主控是哪颗芯片。
- 依赖方向只能向下：`MODULE → INTERFACE`。反向依赖与循环依赖都不允许。

理由不是洁癖：业务逻辑是**唯一在换芯片时不用改的部分**。
一旦它里面出现 `HAL_ADC_GetValue()`，这个性质就没了。

## 命名

- 类型：`typename_t`（如 `fan_state_t`）
- 函数：`module_action()`（如 `fan_control_set_duty()`）
- 宏：`MODULE_DESCRIPTION`（如 `FAN_CONTROL_MAX_DUTY`）
- 文件内函数一律 `static`
- 跨模块取数据走函数（`fan_control_get_duty()`），不用 `extern` 全局变量

完整编码规范见 [`docs/architecture.md`](../docs/architecture.md)。
