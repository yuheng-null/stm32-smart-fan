/**
 * @file pi_control.c
 * @brief 通用 PI 控制器的实现。
 *
 * 算法与取舍见 pi_control.h 顶部。这里只说一处容易写错的地方：
 *
 * **积分器直接以「输出单位」存储，而不是「误差单位」。**
 *   两者在数学上等价（差一个 ki 的因子），但存成输出单位之后，
 *   抗饱和的判别就变得直白：「积分器顶到 out_max 了没有」——
 *   存成误差单位还得再乘一次 ki 才知道，多一步换算就多一处可能写错。
 */

#include "module/pi_control/pi_control.h"

#include <stddef.h>

/**
 * @brief 把值夹到 [lo, hi]。
 *
 * 用两个 if 而不是 fminf/fmaxf：不开 -ffast-math 时两者等价，
 * 但显式写法不依赖 <math.h>，这一层就完全不需要链 libm。
 */
static float clampf(float value, float lo, float hi)
{
    if (value < lo) {
        return lo;
    }
    if (value > hi) {
        return hi;
    }
    return value;
}

/* ----------------------------------------------------------------- 对外 */

error_t pi_init(pi_t *pi, const pi_config_t *cfg)
{
    if ((pi == NULL) || (cfg == NULL)) {
        return ERR_INVALID_PARAM;
    }

    /* 负增益不是「另一种整定」，是把反馈接成了正反馈——直接拒绝，
       免得在后面调参时花时间去追一个永远发散的环路。 */
    if ((cfg->kp < 0.0f) || (cfg->ki < 0.0f)) {
        return ERR_INVALID_PARAM;
    }

    /* 用 !(...) 而不是 >=：若哪天传进 NaN，这里也会判为非法，而 >= 会漏掉它。 */
    if (!(cfg->out_min < cfg->out_max)) {
        return ERR_INVALID_PARAM;
    }

    pi->cfg        = *cfg;
    pi->integrator = 0.0f;
    pi->last_out   = 0.0f;
    pi->primed     = false;   /* 第一拍不做速率限制，见 pi_step_error */

    return ERR_OK;
}

error_t pi_reset(pi_t *pi, float output)
{
    if (pi == NULL) {
        return ERR_INVALID_PARAM;
    }

    /* 预置到「当前正在输出的值」上：下一拍若误差为 0，输出就等于它，
       执行器不会因为切模式而抽一下（无扰切换）。 */
    const float clamped = clampf(output, pi->cfg.out_min, pi->cfg.out_max);

    pi->integrator = clamped;
    pi->last_out   = clamped;
    pi->primed     = true;

    return ERR_OK;
}

error_t pi_set_gains(pi_t *pi, float kp, float ki)
{
    if (pi == NULL) {
        return ERR_INVALID_PARAM;
    }
    if ((kp < 0.0f) || (ki < 0.0f)) {
        return ERR_INVALID_PARAM;
    }

    /* 只动增益：积分器与 last_out 保持原样，调参过程才是连续的。 */
    pi->cfg.kp = kp;
    pi->cfg.ki = ki;

    return ERR_OK;
}

error_t pi_step(pi_t *pi, float setpoint, float measurement, float dt_s, float *out)
{
    if ((pi == NULL) || (out == NULL)) {
        return ERR_INVALID_PARAM;
    }

    return pi_step_error(pi, setpoint - measurement, dt_s, out);
}

error_t pi_step_error(pi_t *pi, float error, float dt_s, float *out)
{
    if ((pi == NULL) || (out == NULL)) {
        return ERR_INVALID_PARAM;
    }
    /* 同理用 !(...) 挡住 dt_s = 0 与 NaN：dt 为 0 时积分增量恒为 0，
       环路悄悄退化成纯比例，而调用方完全看不出。 */
    if (!(dt_s > 0.0f)) {
        return ERR_INVALID_PARAM;
    }

    const float p_term = pi->cfg.kp * error;
    const float i_next = pi->integrator + (pi->cfg.ki * error * dt_s);
    const float unsat  = p_term + i_next;

    float sat = clampf(unsat, pi->cfg.out_min, pi->cfg.out_max);

    /* ---- 抗积分饱和 ---- */
    const bool saturated = (unsat != sat);

    /* 只有「已经饱和」且「误差还在往同侧推」才是该冻结的情形。
       误差已经反向时照常积分——那正是要把积分器拉回来的时刻。 */
    bool pushing_further = false;
    if (saturated) {
        pushing_further = ((unsat > pi->cfg.out_max) && (error > 0.0f))
                       || ((unsat < pi->cfg.out_min) && (error < 0.0f));
    }

    if (pi->cfg.allow_integrate_in_sat || !pushing_further) {
        /* 第二道保险：积分器本身也夹在输出范围内。 */
        pi->integrator = clampf(i_next, pi->cfg.out_min, pi->cfg.out_max);
    }
    /* 否则冻结积分器（条件积分） */

    /* ---- 输出速率限制 ---- */
    /* 第一拍（primed 为假）不限制：那时 last_out 还没有意义，
       从 0 开始限速会让输出凭空从 0 慢慢爬，反而制造一个不存在的软启动。 */
    if (pi->primed && (pi->cfg.out_rate_limit_per_s > 0.0f)) {
        const float max_delta = pi->cfg.out_rate_limit_per_s * dt_s;
        const float delta     = sat - pi->last_out;

        if (delta > max_delta) {
            sat = pi->last_out + max_delta;
        } else if (delta < -max_delta) {
            sat = pi->last_out - max_delta;
        }
    }

    pi->last_out = sat;
    pi->primed   = true;

    *out = sat;

    return ERR_OK;
}

float pi_get_integrator(const pi_t *pi)
{
    return (pi == NULL) ? 0.0f : pi->integrator;
}
