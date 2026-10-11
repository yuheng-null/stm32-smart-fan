/**
 * @file fan_curve.c
 * @brief 风扇曲线（温度 → 目标转速）的实现。
 *
 * 硬件模型与设计取舍见 fan_curve.h 顶部。本文件只讲实现上的两个决定：
 *
 * ── 为什么全程整数 ───────────────────────────────────────────────────
 *   温度（毫摄氏度）与转速本来就是整数，曲线也只是**分段线性**——这种插值和
 *   反查在整数域能做得完全确定，PC 上与目标机上**逐位一致**。
 *   换成浮点的话，同一条曲线在两台机器上可能落在折线的不同侧，边界用例就会
 *   在 CI 上过、在板子上偶尔不过。项目里 fan_control 已经定了这条规矩
 *   （见 fan_control.c 顶部注释），这里沿用。
 *
 * ── 为什么迟滞做在「温度」上而不是「转速」上 ─────────────────────────
 *   被控量是温度，抖动也来自温度（ADC 噪声、环境扰动）。把迟滞定义成
 *   「温度得挪开 1 ℃ 才认」比「转速得差 200 才认」更贴近物理，
 *   而且与曲线斜率无关——曲线陡的地方不会因此变得迟钝。
 * ────────────────────────────────────────────────────────────────────
 */

#include "module/fan_curve/fan_curve.h"

#include <stddef.h>

/**
 * @brief 分段线性插值：温度 → 名义转速（不含迟滞）。
 *
 * 端点外按配置的常量取值，不外推——外推在曲线尽头很容易给出离谱的转速，
 * 而那里本来就没有数据支撑。
 */
static uint32_t curve_interp_rpm(const fan_curve_t *curve, int32_t temp_milli_c)
{
    const fan_curve_config_t *cfg = &curve->cfg;
    const fan_curve_point_t  *pts = cfg->points;
    const uint8_t             n   = cfg->count;

    if (temp_milli_c <= pts[0].temp_milli_c) {
        return cfg->rpm_at_or_below_min;
    }
    if (temp_milli_c >= pts[n - 1U].temp_milli_c) {
        return cfg->rpm_at_or_above_max;
    }

    for (uint8_t i = 0U; i < (uint8_t)(n - 1U); i++) {
        if (temp_milli_c <= pts[i + 1U].temp_milli_c) {
            /* 全部用 64 位中间量：毫摄氏度 × 转速的乘积很容易越过 32 位。
               d_temp 必定 > 0 —— init 已校验温度锚点严格升序，这里不会除零。 */
            const int64_t d_temp = (int64_t)pts[i + 1U].temp_milli_c - (int64_t)pts[i].temp_milli_c;
            const int64_t d_rpm  = (int64_t)pts[i + 1U].rpm - (int64_t)pts[i].rpm;
            const int64_t span   = (int64_t)temp_milli_c - (int64_t)pts[i].temp_milli_c;
            const int64_t value  = (int64_t)pts[i].rpm + (span * d_rpm) / d_temp;

            /* 曲线本身允许转速非单调，插值结果因此可能为负；转速没有负值，
               夹到 0 而不是回绕成一个巨大的正数。 */
            return (value < 0) ? 0U : (uint32_t)value;
        }
    }

    return cfg->rpm_at_or_above_max;    /* 循环内必定命中，此处不可达 */
}

/**
 * @brief 反查：某个转速对应曲线上的哪个温度。
 *
 * 迟滞需要它——「上次输出对应的温度」就是迟滞带的中心点。
 *
 * 曲线转速非单调时反查不唯一，本函数取**第一个**命中的分段，
 * 结果仍有界、不会发散。对本工程用的单调升曲线，结果唯一。
 */
static int32_t curve_temp_at_rpm(const fan_curve_t *curve, uint32_t rpm)
{
    const fan_curve_config_t *cfg = &curve->cfg;
    const fan_curve_point_t  *pts = cfg->points;
    const uint8_t             n   = cfg->count;

    if (rpm <= pts[0].rpm) {
        return pts[0].temp_milli_c;
    }
    if (rpm >= pts[n - 1U].rpm) {
        return pts[n - 1U].temp_milli_c;
    }

    for (uint8_t i = 0U; i < (uint8_t)(n - 1U); i++) {
        if (rpm <= pts[i + 1U].rpm) {
            const int64_t d_rpm = (int64_t)pts[i + 1U].rpm - (int64_t)pts[i].rpm;

            /* 平台段（相邻两点转速相同）：反查不唯一，取左端温度。
               此时迟滞带落在平台内，行为仍然确定。 */
            if (d_rpm <= 0) {
                return pts[i].temp_milli_c;
            }

            const int64_t d_temp = (int64_t)pts[i + 1U].temp_milli_c - (int64_t)pts[i].temp_milli_c;
            const int64_t span   = (int64_t)rpm - (int64_t)pts[i].rpm;

            return pts[i].temp_milli_c + (int32_t)((span * d_temp) / d_rpm);
        }
    }

    return pts[n - 1U].temp_milli_c;    /* 不可达 */
}

/* ----------------------------------------------------------------- 对外 */

error_t fan_curve_init(fan_curve_t *curve, const fan_curve_config_t *cfg)
{
    if ((curve == NULL) || (cfg == NULL) || (cfg->points == NULL)) {
        return ERR_INVALID_PARAM;
    }

    if (cfg->count < 2U) {
        /* 一个点连不成折线。与其在求值时退化成常数，不如在这里就拒绝。 */
        return ERR_INVALID_PARAM;
    }

    if (cfg->hysteresis_milli_c < 0) {
        /* 负迟滞会让「升速」与「降速」的条件自相矛盾，无意义。 */
        return ERR_INVALID_PARAM;
    }

    /* 温度锚点必须**严格**升序：相等会让插值出现零除，逆序会让反查失去意义。
       这个校验只做一次，之后就靠它保证 curve_interp_rpm() 里的除法安全。 */
    for (uint8_t i = 1U; i < cfg->count; i++) {
        if (cfg->points[i].temp_milli_c <= cfg->points[i - 1U].temp_milli_c) {
            return ERR_INVALID_PARAM;
        }
    }

    curve->cfg      = *cfg;     /* 复制配置；points 仍指向调用者的数组，不复制内容 */
    curve->primed   = false;
    curve->last_rpm = 0U;

    return ERR_OK;
}

error_t fan_curve_eval(fan_curve_t *curve, int32_t temp_milli_c, uint32_t *target_rpm)
{
    if ((curve == NULL) || (target_rpm == NULL)) {
        return ERR_INVALID_PARAM;
    }

    /* 「装过没有」用 points 是否为空判断：零初始化的实例必然是 NULL。
       不额外加 ready 标志，是为了让实例可以被 memset 清零后安全使用。 */
    if ((curve->cfg.points == NULL) || (curve->cfg.count < 2U)) {
        return ERR_NOT_INITIALIZED;
    }

    const uint32_t nominal = curve_interp_rpm(curve, temp_milli_c);

    /* 首次求值没有参考点，无条件接受 —— 否则第一拍的迟滞无从比较。 */
    if (!curve->primed) {
        curve->last_rpm = nominal;
        curve->primed   = true;
        *target_rpm     = nominal;
        return ERR_OK;
    }

    const uint32_t ref = curve->last_rpm;

    if (nominal != ref) {
        /* 迟滞带的中心是「当前输出对应的温度」，不是曲线的某个锚点：
           这样换挡点会跟着当前工作点走，而不是把温度硬拽到锚点上。 */
        const int32_t ref_temp = curve_temp_at_rpm(curve, ref);
        const int32_t hyst     = curve->cfg.hysteresis_milli_c;

        const bool commit = (nominal > ref) ? (temp_milli_c >= (ref_temp + hyst))
                                            : (temp_milli_c <= (ref_temp - hyst));

        if (commit) {
            curve->last_rpm = nominal;
        }
    }

    *target_rpm = curve->last_rpm;

    return ERR_OK;
}

void fan_curve_reset(fan_curve_t *curve)
{
    if (curve == NULL) {
        return;
    }

    /* 只丢记忆，不动配置——复位之后这条曲线仍然是「装好的」。 */
    curve->primed   = false;
    curve->last_rpm = 0U;
}
