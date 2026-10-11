/**
 * @file fan_control.c
 * @brief 风扇控制模块的实现（纯计算，无硬件）。
 */

#include "module/fan_control/fan_control.h"

#include <stddef.h>

/*
 * 60 / (T_秒 × 每转脉冲数)，T 用微秒表示后把 1e-6 提到分子上：
 *     60 × 1e6 / 2 = 30e6
 * 用整数常量而不是写成除法链，是为了避免浮点——这颗 MCU 上浮点方便，
 * 但业务层用整数能保证在任何平台上逐位一致，也便于在 PC 上单测。
 */
#define FAN_RPM_NUMERATOR   (60000000UL / FAN_CONTROL_PULSES_PER_REV)

error_t fan_control_rpm_from_period_us(uint32_t period_us, uint32_t *rpm)
{
    if (rpm == NULL) {
        return ERR_INVALID_PARAM;
    }

    /*
     * period_us == 0 是无效读数，不是「转速无穷大」：
     * 两次捕获读到同一个计数，说明两个沿之间不足 1 µs ——
     * 对风扇来说不可能（那会是 30e6 RPM），只可能是捕获到了噪声毛刺。
     * 直接拒绝，让调用方沿用上一次的有效值，而不是把毛刺算成天文数字。
     */
    if (period_us == 0U) {
        return ERR_INVALID_PARAM;
    }

    const uint32_t value = FAN_RPM_NUMERATOR / period_us;

    /*
     * 间隔长到不足 1 RPM（period_us > 30e6，即 30 秒半转）——风扇不可能这么慢，
     * 实际含义是「转得很慢或已经停了」。返回 UNDEFLOW 让调用方明确按 0 处理，
     * 而不是拿到一个悄悄变成 0 的值分不清是慢还是出错。
     */
    if (value == 0U) {
        return ERR_UNDERFLOW;
    }

    /*
     * 另一头也要挡：间隔太短说明这"两个沿"根本不是相邻的两个真实脉冲，
     * 而是同一条线上的毛刺。放过去的话，屏幕上会出现几十万转的读数。
     */
    if (value > FAN_CONTROL_RPM_MAX) {
        return ERR_OVERFLOW;
    }

    *rpm = value;

    return ERR_OK;
}

uint8_t fan_control_duty_step(uint8_t duty, fan_duty_dir_t dir)
{
    /*
     * 先把入参夹进合法区间：调用方可能传入越界值（比如从串口直接解析来的），
     * 这里兜住比在外面每一处都判断一遍更不容易漏。
     */
    if (duty > FAN_CONTROL_DUTY_MAX_PERCENT) {
        duty = FAN_CONTROL_DUTY_MAX_PERCENT;
    }

    if (dir == FAN_DUTY_UP) {
        /* 用「到上限还差多少」来夹，避免 duty + STEP 在 250 附近溢出后回绕成小值 */
        const uint8_t room = (uint8_t)(FAN_CONTROL_DUTY_MAX_PERCENT - duty);

        return (room < FAN_CONTROL_DUTY_STEP_PERCENT)
                   ? FAN_CONTROL_DUTY_MAX_PERCENT
                   : (uint8_t)(duty + FAN_CONTROL_DUTY_STEP_PERCENT);
    }

    return (duty < FAN_CONTROL_DUTY_STEP_PERCENT)
               ? FAN_CONTROL_DUTY_MIN_PERCENT
               : (uint8_t)(duty - FAN_CONTROL_DUTY_STEP_PERCENT);
}
