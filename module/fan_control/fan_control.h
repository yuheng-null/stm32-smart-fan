/**
 * @file fan_control.h
 * @brief 风扇控制模块：FG 测速换算 + 占空比步进策略。
 *
 * 本文件是**纯计算**——不含 HAL、不含寄存器、不含任何硬件访问，
 * 输入是「两个 FG 上升沿之间数了多少个 1 µs 计数」和「当前占空比」，
 * 输出是转速和下一档占空比。因此它可以在 PC 上直接做单元测试。
 *
 * 硬件动作（拉高拉低、起停定时器）在 bsp/bsp_fan.c 里，本模块只管算法。
 *
 * ── 转速公式的来历 ──────────────────────────────────────────────────
 *   Intel 4-Wire PWM 规范：FG（测速）输出**每转 2 个脉冲**。
 *   相邻两个上升沿之间正好是半个机械转，即周期 T 对应 1/2 转：
 *
 *       RPM = 60 / (T_秒 × 2)
 *
 *   bsp/bsp_fan.c 把 TIM2 配成 1 MHz 计数（1 个计数 = 1 µs），
 *   于是代入 T_秒 = T_us × 1e-6：
 *
 *       RPM = 60 / (T_us × 1e-6 × 2) = 30 000 000 / T_us
 *
 *   例：1000 RPM → 半转 30 ms → T_us = 30000 → 30e6 / 30000 = 1000 ✓
 *
 * ⚠️ 这个 2 是**规范值**，不是本风扇实测的。若换了非标风扇，
 *    先用已知转速标定一次：RPM_实际 / RPM_本函数 = 每转脉冲数 / 2。
 */
#ifndef MODULE_FAN_CONTROL_H
#define MODULE_FAN_CONTROL_H

#include <stdbool.h>
#include <stdint.h>

#include "interface/error.h"

/** FG 每转脉冲数（Intel 4 线规范：2）。⚠️ 换非标风扇需重新标定。 */
#define FAN_CONTROL_PULSES_PER_REV  (2U)

/** 占空比范围（百分比）。 */
#define FAN_CONTROL_DUTY_MIN_PERCENT    (0U)
#define FAN_CONTROL_DUTY_MAX_PERCENT    (100U)

/** 一次「加档 / 减档」改变的百分比。 */
#define FAN_CONTROL_DUTY_STEP_PERCENT   (10U)

/**
 * 转速的合理上限（转/分）。
 *
 * **不是物理极限，是"这读数不该被当真"的判据。** 12V 的 4 线风扇实际满速
 * 也就几千转，而 FG 线上一个几十纳秒的毛刺会让捕获间隔小到算出天文数字
 * （间隔 1 µs → 30 000 000 RPM）。超过这个上限一律判为噪声，返回
 * ERR_OVERFLOW，让调用方沿用上一次的有效值，而不是把毛刺画到屏幕上。
 */
#define FAN_CONTROL_RPM_MAX             (30000U)

/**
 * ⚠️ 4 线风扇规范写明：占空比 **低于 20% 时转速是不确定的**
 * （有的停转、有的维持最低转速，厂商自定）。
 * 所以 20% 以下请只当「能停住」用，不要指望它与转速成比例。
 */
#define FAN_CONTROL_DUTY_SPEC_MIN_PERCENT   (20U)

/**
 * ── 占空比 ⇄ 转速的经验模型（本风扇实测）─────────────────────────────
 *   依据 README 记录的实测点：占空比 20% / 50% / 100% → 观测转速
 *   3347 / 8527 / 16456 rpm。最小二乘拟合得
 *
 *       rpm ≈ 163 × duty% + 190          （拟合偏差约 2%）
 *
 *   ⚠️ 这组常数是**本风扇、本风道**的实测值，换风扇必须重测。
 *   ⚠️ 转速是「观测单位」：若每转脉冲数标定值与 2 不符，本组常数要
 *      **按同一倍率缩放**（否则前馈与闭环会各说各话）。
 *   ⚠️ 该模型只在 ≥20% 有实测支撑——20% 以下 4 线规范未定义，实测也
 *      偏离直线（0% 时风扇仍维持约 295 rpm）。
 */
#define FAN_CONTROL_RPM_PER_PERCENT (163)   /**< 静态增益 K（观测 rpm / %） */
#define FAN_CONTROL_RPM_INTERCEPT   (190)   /**< 截距（观测 rpm） */

/** 占空比步进方向。 */
typedef enum {
    FAN_DUTY_UP = 0,    /**< 加一档 */
    FAN_DUTY_DOWN,      /**< 减一档 */
} fan_duty_dir_t;

/**
 * @brief 由 FG 相邻两上升沿的间隔算转速。
 *
 * @param period_us 间隔，单位微秒（TIM2 配成 1 MHz 计数，故计数即微秒）。
 *                  必须非 0——为 0 说明两个沿落在同一个计数上，是无效读数。
 * @param rpm       输出，转速（转/分）。
 *
 * @return error_t ERR_OK 正常；
 *                 ERR_INVALID_PARAM rpm 为空或 period_us 为 0；
 *                 ERR_UNDERFLOW 间隔大到算出的转速不足 1 RPM（极慢或已停转，
 *                               调用方应把它当 0 处理）；
 *                 ERR_OVERFLOW  算出的转速超过 FAN_CONTROL_RPM_MAX
 *                               （FG 线上的毛刺，不是真实转速）。
 */
error_t fan_control_rpm_from_period_us(uint32_t period_us, uint32_t *rpm);

/**
 * @brief 占空比加/减一档，并夹在 [0, 100] 内。
 *
 * @param duty 当前占空比（百分比）。超出范围时会先被夹到合法区间。
 * @param dir  方向。
 *
 * @return uint8_t 新的占空比（百分比）。
 */
uint8_t fan_control_duty_step(uint8_t duty, fan_duty_dir_t dir);

/**
 * @brief 由目标转速反算一个**名义占空比**（经验模型反解）。
 *
 * 用途是**前馈与开环兜底**，不是替代内环 PI：
 *   · 前馈：给 PI 一个接近的起点，减少它从零开始爬的时间；
 *   · 开环兜底：FG 线断了、内环失去反馈时，仍能按温度曲线大致给出风量
 *     （见 module/thermal_control）。
 *
 * @param rpm           目标转速（观测单位）。
 * @param duty_percent  输出，名义占空比，夹在 [0, 100]。
 *
 * @return error_t ERR_OK 正常；
 *                 ERR_INVALID_PARAM duty_percent 为空；
 *                 ERR_UNDERFLOW rpm 低到不落在模型的有效范围内
 *                               （≤ 截距；此时输出 0，表示「按停机处理」）。
 */
error_t fan_control_duty_from_rpm(uint32_t rpm, uint8_t *duty_percent);

#endif /* MODULE_FAN_CONTROL_H */
