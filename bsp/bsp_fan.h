/**
 * @file bsp_fan.h
 * @brief 板级风扇接口：PWM 调速（PA8）+ FG 测速（PA1）。
 *
 * ── 接线 ────────────────────────────────────────────────────────────
 *   风扇 PWM 线 ── PA8（TIM1_CH1，25 kHz，**开漏**）
 *   风扇 FG  线 ── PA1（TIM2_CH2，输入捕获，内部上拉）
 *   风扇 12V/GND ── 独立 12V 电源，**GND 必须与板子共地**
 * ────────────────────────────────────────────────────────────────────
 *
 * ── 为什么 PWM 用开漏 ────────────────────────────────────────────────
 *   4 线风扇的 PWM 脚内部自带上拉（Intel 4-Wire PWM 规范：拉到 3.3V 或 5V，
 *   开路电压上限 5.25V），主板侧只负责拉低。用开漏驱动，高电平由风扇自己
 *   建立，不管它上拉到 3.3V 还是 5V 都不会和 MCU 的输出级对顶。
 *   佐证：PWM 悬空时风扇会全速转 —— 那正是「被内部上拉拉高 = 100%」。
 *   PA8 在数据手册里是 I/O FT（5V 耐受），5.25V 也在范围内。
 *
 * ── 为什么占空比 0 能停住风扇 ────────────────────────────────────────
 *   开漏 + 占空比 0 = 引脚被 MCU 一直拉低，风扇内部上拉被压住，
 *   PWM 脚恒为低 → 风扇停（或维持最低转速）。
 *   **这恰好是「先跑 PWM 再给风扇上电」能成立的原因**：
 *   反过来先上电、板子还没跑，PWM 悬空 = 100% = 全速。
 *
 * ── 测速侧 ──────────────────────────────────────────────────────────
 *   FG 是**开集输出**，正常风扇不带内部上拉，靠主板拉。这里启用 STM32 的
 *   内部上拉（约 40 kΩ）兜底。
 *   ⚠️ 少数风扇的 FG 内部上拉到 5V/12V —— 那种直连会打坏 PA1，
 *      接线前必须用万用表实测 FG 对 GND 的电压（见 docs/hardware.md）。
 *
 *   本层只吐出**原始周期（µs）**，不吐转速：转速换算属于业务逻辑，
 *   在 module/fan_control/ 里。BSP 不能反向依赖 MODULE。
 */
#ifndef BSP_FAN_H
#define BSP_FAN_H

#include <stdint.h>

#include "interface/error.h"

/**
 * @brief 启动风扇 PWM 与 FG 输入捕获。
 *
 * 占空比初始化为 0（风扇停住）——上电不应该有意外动作。
 * 必须在 CubeMX 生成的 MX_TIM1_Init() / MX_TIM2_Init() 之后调用。
 *
 * @return error_t ERR_OK 成功。
 */
error_t bsp_fan_init(void);

/**
 * @brief 设置 PWM 占空比。
 *
 * @param percent 0~100（百分比）。超出范围返回 ERR_INVALID_PARAM。
 *                ⚠️ 4 线风扇规范：低于 20% 时转速不确定，只当「能停住」用。
 *
 * @return error_t ERR_OK；ERR_INVALID_PARAM 越界或未初始化。
 */
error_t bsp_fan_set_duty_percent(uint8_t percent);

/**
 * @brief 取当前占空比。
 *
 * @return uint8_t 百分比；未初始化时返回 0。
 */
uint8_t bsp_fan_get_duty_percent(void);

/**
 * @brief 取最近一次测得的两条 FG 上升沿间隔。
 *
 * @param period_us 输出，单位微秒（TIM2 配成 1 MHz 计数，故计数即微秒）。
 *
 * @return error_t ERR_OK 正常；
 *                 ERR_INVALID_PARAM 指针为空；
 *                 ERR_NOT_INITIALIZED 还没调用 bsp_fan_init()；
 *                 ERR_NOT_READY 只捕获到一个沿，还凑不出间隔；
 *                 ERR_TIMEOUT 超过 BSP_FAN_TACH_TIMEOUT_MS 没有新沿
 *                             （风扇停转或 FG 线没接）——
 *                             调用方应把它当作转速 0。
 */
error_t bsp_fan_get_period_us(uint32_t *period_us);

/**
 * @brief 累计捕获到的 FG 沿个数（调试用）。
 *
 * 它是判断「FG 线到底有没有信号」最直接的量：
 * 风扇在转却始终为 0，说明线没接好或没接上拉；
 * 数值飞快上涨则说明线路在抖（噪声被当成了沿）。
 *
 * @return uint32_t 累计沿数。
 */
uint32_t bsp_fan_get_capture_count(void);

/** 诊断时选择强制拉低哪一根引脚。 */
typedef enum {
    BSP_FAN_FORCE_PWM = 0,   /**< 只拉低 PA8（风扇 PWM 脚） */
    BSP_FAN_FORCE_TACH,      /**< 只拉低 PA1（风扇测速脚） */
    BSP_FAN_FORCE_BOTH,      /**< 两根都拉低 */
} bsp_fan_force_t;

/**
 * @brief **诊断用**：绕过定时器，把选定的引脚强制成普通推挽输出并拉低。
 *
 * 为什么要这个：正常路径上，PA8 出波形要经过「AF1 复用 → 开漏输出级 → MOE」
 * 好几环，任何一环没生效，引脚都会停在**释放**状态，而现象和「线没接对」
 * 完全一样，分不出是谁的问题。用普通 GPIO 直接拉低就把这些中间环节短路掉。
 *
 * 分三次而不是一次拉低两根，是为了**定位是哪一根在起作用**：
 *   · 只拉低 PA8 风扇就停 → PA8 确实是 PWM 线，问题在定时器/AF 这条路；
 *   · 只拉低 PA8 不停、只拉低 PA1 才停 → **两根信号线接反了**
 *     （PA1 上挂的才是风扇的 PWM 输入，它的内部上拉一直把风扇顶在全速）。
 *
 * 实测（2026-10-11）：两根同时拉低时风扇停住 —— 这条首先排除了
 * 「线没接对」和「没共地」，把问题锁定在固件侧。
 *
 * ⚠️ 调用后对应的定时器停止，`bsp_fan_get_period_us()` 可能返回
 *    ERR_NOT_INITIALIZED。**恢复要复位板子**（重新跑一遍生成的初始化）。
 */
void bsp_fan_force_pin_low(bsp_fan_force_t which);

#endif /* BSP_FAN_H */
