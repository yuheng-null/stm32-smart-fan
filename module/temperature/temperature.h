/**
 * @file temperature.h
 * @brief 温度模块：把 NTC 分压的 ADC 原始计数换算成摄氏温度。
 *
 * 本文件是**纯计算**——不含 HAL、不含寄存器、不含任何硬件访问，
 * 输入是一个 ADC 计数，输出是一个温度值。因此它可以在 PC 上直接做单元测试。
 * 采样动作在 bsp/bsp_adc.c 里，本模块只管「这个数字对应多少度」。
 *
 * ── 硬件模型 ────────────────────────────────────────────────────────
 *   LM393 型 NTC 模块的 AO 引脚，取的是模块上一只**固定电阻**与
 *   **NTC 热敏电阻**串联分压的中点：
 *
 *        3V3 ──[ R_FIXED ]──┬── AO（接 PA0）
 *                           │
 *                       [ R_NTC ]
 *                           │
 *                          GND
 *
 *   温度升高 → R_NTC 变小 → AO 电压下降。这与实测现象一致
 *   （见 README 的接线说明）。所以：
 *
 *        V_AO = VCC · R_NTC / (R_FIXED + R_NTC)
 *    =>   R_NTC = R_FIXED · V_AO / (VCC − V_AO)
 *
 * ── 为什么这里能把 VCC 消掉 ─────────────────────────────────────────
 *   模块 VCC 与 ADC 的 VREF+ 取自同一块板子的 3.3V 轨，于是
 *   V_AO/VCC 正好等于 raw/(2^12 − 1)，VCC 的绝对值消掉了：
 *
 *        R_NTC = R_FIXED · raw / (4095 − raw)
 *
 *   这是「模块必须用 3.3V 供电」（见 bsp_adc.h）带来的一个附带好处：
 *   3.3V 轨的实际电压有 ±2% 波动也不影响换算结果。
 *   ⚠️ 若哪天把模块改接到 5V，这个式子立刻失效，同时也会削顶、过压。
 *
 * ── 参数是假设值，必须校准 ──────────────────────────────────────────
 *   ⚠️ NTC_R0 / NTC_B / TEMPERATURE_DIVIDER_R_OHM 三个常数**不是实测的**，
 *      依据是这类模块的常见配置（10 kΩ 分压电阻 + MF52 10 kΩ B=3950）。
 *      模块厂商不同、批次不同，这三个值都可能不一样，而它们直接决定
 *      绝对精度——参数不对会出现「室温下读 15℃」这种整体偏移，
 *      但**曲线形状仍然是对的**（升温还会涨）。
 *
 *   校准方法（把三处常数改准即可，改完重新编译烧录）：
 *     1. 记下室温 T1 与此时页面显示的计数 raw1；
 *     2. 换一个明显不同的温度 T2（冰水 0℃ 或热水），记下 raw2；
 *     3. 由上面那条公式反解出两点的 R_NTC，再用两点解 Beta 方程，
 *        即可同时得到 R0 与 B（联立两个 1/T = 1/T0 + ln(R/R0)/B）。
 *
 *   校准前 display 上的温度只应当当作**相对趋势**看，不是准确值。
 */
#ifndef MODULE_TEMPERATURE_H
#define MODULE_TEMPERATURE_H

#include <stdint.h>

#include "interface/error.h"

/** NTC 在 25 ℃ 时的标称阻值（欧姆）。⚠️ 假设值，需校准。 */
#define TEMPERATURE_NTC_R0_OHM      (10000.0f)

/** NTC 的 B 值（K）。⚠️ 假设值，需校准。 */
#define TEMPERATURE_NTC_B           (3950.0f)

/** 标称温度 T0，即 R0 对应的绝对温度：25 ℃ = 298.15 K。 */
#define TEMPERATURE_NTC_T0_K        (298.15f)

/** 模块上与 NTC 串联的那只固定电阻（欧姆）。⚠️ 假设值，需校准。 */
#define TEMPERATURE_DIVIDER_R_OHM   (10000.0f)

/** 12 位 ADC 的满量程计数。 */
#define TEMPERATURE_ADC_FULL_SCALE  (4095)

/*
 * 有效计数窗口。
 *
 * 端点必须排除，原因在数学上：raw=0 时 R_NTC 算出来是 0（短路），
 * raw=4095 时是无穷大（开路），两者都会让对数运算发散。
 * 物理上它们分别对应「AO 被拉到地」和「AO 被拉到 VCC」，
 * 也就是短路与断路，都不是正常测温状态。
 */
#define TEMPERATURE_RAW_MIN         (1U)
#define TEMPERATURE_RAW_MAX         (4094U)

/**
 * @brief 由 ADC 原始计数算出 NTC 阻值。
 *
 * 调试与校准用：拿它和万用表量到的阻值对一对，就知道分压电阻那类
 * 假设值对不对。
 *
 * @param raw ADC 原始计数。
 *
 * @return uint32_t NTC 阻值（欧姆）；raw 越界时返回 0。
 */
uint32_t temperature_resistance_from_raw(uint16_t raw);

/**
 * @brief 由 ADC 原始计数算出摄氏温度。
 *
 * 用 Beta 方程：1/T = 1/T0 + ln(R/R0)/B。
 *
 * @param raw     ADC 原始计数（建议先经 bsp_adc_read_averaged() 平均）。
 * @param milli_c 输出，温度，单位**毫摄氏度**（整数），避免用浮点 printf。
 *
 * @return error_t ERR_OK 正常；
 *                 ERR_INVALID_PARAM milli_c 为空；
 *                 ERR_UNDERFLOW raw 低到量程尽头（AO 被拉低：短路或过热）；
 *                 ERR_OVERFLOW  raw 高到量程尽头（AO 被拉高：断路或过冷）。
 *                 后两种情况 milli_c 里放的是该端点的温度值。
 */
error_t temperature_from_adc_raw(uint16_t raw, int32_t *milli_c);

#endif /* MODULE_TEMPERATURE_H */
