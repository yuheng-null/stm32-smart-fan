/**
 * @file thermal_control.c
 * @brief 温控级联控制器的实现。
 *
 * 级联结构与设计理由见 thermal_control.h 顶部。本文件重点讲三处「非平凡」的决定：
 *
 * ── 1. dt 用真实 tick 差，并且夹上限 ─────────────────────────────────
 *   主循环里 OLED 刷新要搬 1 KB 软件 I2C（几十毫秒），所以控制节拍**必然**
 *   会有抖动。用真实 dt 之后抖动被自动补偿；再夹一个上限，是为了挡住极端情况：
 *   若主循环被阻塞了一整秒，一拍吃进 1 s 的积分量会让输出直接甩出去。
 *
 * ── 2. 转速丢失时切「开环前馈」，而不是停或全速 ──────────────────────
 *   内环靠 FG 反馈。线断了以后有两条偷懒的路：卡在最后一次输出（风扇状态
 *   与温度脱钩）、或直接满速（噪音大且没必要）。两条都不对——**温度传感器
 *   还好好的**，只是不知道转速，那就按曲线给的目标转速经经验模型反算一个
 *   占空比直接开环输出，风扇仍随温度变化，只是精度差些。
 *
 * ── 3. 去抖只在这里做一次 ────────────────────────────────────────────
 *   状态机消费的是本模块输出的、已去抖的 in_temp_fallback / tach_lost。
 *   两处各去抖一遍会让「到底延迟了几拍」说不清、也难写测试。
 * ────────────────────────────────────────────────────────────────────
 */

#include "module/thermal_control/thermal_control.h"

#include <stddef.h>

#include "module/fan_control/fan_control.h"

/** 内环「追不上目标」的判据：偏差持续超过目标值的这个比例… */
#define THERMAL_NOT_TRACKING_RATIO      (0.20f)

/** …并且持续这么多拍（30 × 100 ms = 3 s）才报警，避免启动瞬态误报。 */
#define THERMAL_NOT_TRACKING_SAMPLES    (30U)

/** dt 的上限 = 标称周期的这个倍数。 */
#define THERMAL_DT_MAX_PERIODS          (10U)

/**
 * @brief 把浮点占空比落成整数百分比，并做有效下限处理。
 *
 * 4 线风扇规范：占空比低于 20% 时转速**未定义**（本工程实测 0% 时风扇仍维持
 * 约 295 rpm）。所以输出要么是 0（停机档），要么 ≥ duty_min，中间那段不碰——
 * 否则会出现「指令 8%，风扇却突然全速」这种没法解释的现象。
 */
static uint8_t duty_to_percent(float duty_f, uint8_t duty_min_effective)
{
    /* +0.5 再截断 = 四舍五入。负值也走这条路：-0.6 → -0.1 → 0。 */
    int32_t d = (int32_t)(duty_f + 0.5f);

    if (d < 0) {
        d = 0;
    } else if (d > 100) {
        d = 100;
    }

    if ((d > 0) && (d < (int32_t)duty_min_effective)) {
        d = (int32_t)duty_min_effective;
    }

    return (uint8_t)d;
}

/**
 * @brief 「占空比下限所能达到的最低转速」——物理可达区间的下沿。
 *
 * 4 线风扇在 <20%（`duty_min_effective_percent`）时行为未定义，所以实际能用
 * 的转速只有 `{0} ∪ [本值, 满速]`。曲线是连续的，低端必然经过一段**不可达**
 * 的转速，那一段必须被抬上来。
 *
 * 依据 `fan_control` 的经验模型：rpm ≈ 163 × duty% + 190 → 20% 时约 3450。
 */
static uint32_t thermal_min_reachable_rpm(uint8_t duty_min_percent)
{
    return (uint32_t)(((uint32_t)FAN_CONTROL_RPM_PER_PERCENT * (uint32_t)duty_min_percent)
                      + (uint32_t)FAN_CONTROL_RPM_INTERCEPT);
}

/**
 * @brief 算本拍的真实时长（秒）。
 *
 * 首拍没有参考点，退化为标称周期；同一 tick 内被调用两次也退化为标称周期
 * （否则 dt = 0 会让积分量为 0，环路悄悄少走一拍）。
 */
static float thermal_control_dt_s(const thermal_controller_t *ctl, uint32_t now_ms)
{
    const uint32_t nominal_ms = ctl->cfg.control_period_ms;
    uint32_t       dt_ms;

    if (!ctl->primed) {
        dt_ms = nominal_ms;
    } else {
        /* 无符号减法：tick 回绕（约 49.7 天）时结果依然正确。 */
        dt_ms = now_ms - ctl->last_ms;
        if (dt_ms == 0U) {
            dt_ms = nominal_ms;
        }
    }

    const uint32_t cap_ms = nominal_ms * (uint32_t)THERMAL_DT_MAX_PERIODS;
    if (dt_ms > cap_ms) {
        dt_ms = cap_ms;
    }

    return (float)dt_ms / 1000.0f;
}

/**
 * @brief 推进一个「连续 N 次才改变」的去抖计数器。
 *
 * @param streak   计数器的当前值（会被更新）。
 * @param observed 本拍观察到的状态。
 * @param want     当前认定的状态（会被更新）。
 * @param need     需要连续多少次才翻转。
 */
/**
 * @brief 这个转速读数是否算「FG 信号确实断了」的证据。
 *
 * 只有两种算：
 *   · ERR_TIMEOUT   —— 超过 1 s 没有新沿（bsp 内部已经在计时，所以这一条
 *                      本身就已经是相当确定的信号）；
 *   · ERR_UNDERFLOW —— 间隔长到不足 1 RPM，实质等同停转。
 *
 * **不算**的两种，都会在风扇每次启动时凭空报一次故障：
 *   · ERR_NOT_READY —— 刚起转、只捕获到一个沿，还没凑出间隔（正常瞬态）；
 *   · ERR_OVERFLOW  —— FG 线上的毛刺，本来就该被忽略。
 */
static bool tach_evidence_lost(bool valid, error_t status)
{
    if (valid) {
        return false;
    }

    return (status == ERR_TIMEOUT) || (status == ERR_UNDERFLOW);
}

static void debounce_step(uint16_t *streak, bool observed, bool *want, uint16_t need)
{
    if (observed == *want) {
        *streak = 0U;       /* 与当前认定一致，清掉相反方向的计数 */
        return;
    }

    if (*streak < UINT16_MAX) {
        (*streak)++;
    }

    if (*streak >= need) {
        *want   = observed;
        *streak = 0U;
    }
}

/* ----------------------------------------------------------------- 对外 */

error_t thermal_control_init(thermal_controller_t *ctl, const thermal_config_t *cfg)
{
    if ((ctl == NULL) || (cfg == NULL)) {
        return ERR_INVALID_PARAM;
    }
    if (cfg->control_period_ms == 0U) {
        return ERR_INVALID_PARAM;
    }
    if (cfg->duty_min_effective_percent > 100U) {
        return ERR_INVALID_PARAM;
    }

    /* 先整体清零：保证没有任何字段带着上一次的残留值进来。 */
    *ctl = (thermal_controller_t){0};

    ctl->cfg = *cfg;

    /* 去抖次数归一化：0 视为「立刻生效」（1 拍），免得写 0 的人以为关掉了去抖，
       实际得到的却是「永远不生效」。 */
    if (ctl->cfg.invalid_debounce_samples == 0U) {
        ctl->cfg.invalid_debounce_samples = 1U;
    }
    if (ctl->cfg.recovery_debounce_samples == 0U) {
        ctl->cfg.recovery_debounce_samples = 1U;
    }

    error_t err = fan_curve_init(&ctl->curve_state, &ctl->cfg.curve);
    if (err != ERR_OK) {
        return err;
    }

    err = pi_init(&ctl->pi_state, &ctl->cfg.pi);
    if (err != ERR_OK) {
        return err;
    }

    /*
     * 把「有效占空比下限」下沉到 PI 的输出范围里。
     *
     * 为什么不能只在最后夹一下：PI 一旦被允许输出 17%，而外层把它抬到 20%，
     * 风扇就会转得比 PI 以为的快；PI 于是把输出压向 0，风扇停；转速掉下来，
     * PI 又把它抬上去——**极限环**，现象是转速周期性地上蹿下跳。
     * 把下限交给 PI 自己，它就会老老实实饱和在 20% 上不动
     * （并触发 RPM_NOT_TRACKING，如实报告「够不到目标」）。
     *
     * 取两者较大值：调用方若在 pi.out_min 里写了更保守的下限，尊重它。
     */
    if ((float)ctl->cfg.duty_min_effective_percent > ctl->pi_state.cfg.out_min) {
        ctl->pi_state.cfg.out_min = (float)ctl->cfg.duty_min_effective_percent;
    }

    /*
     * 抬高之后必须重新校验区间：pi_init() 只验过**原样**的 out_min < out_max，
     * 若调用方给的 out_max 还小于 duty_min（例如 duty_min=100、out_max=50），
     * 这里就会造出一个 out_min > out_max 的 PI——clampf() 会把任何输入都夹成
     * out_min，输出被钉死，而环路上看不出任何异常。
     */
    if (!(ctl->pi_state.cfg.out_min < ctl->pi_state.cfg.out_max)) {
        return ERR_INVALID_PARAM;
    }

    ctl->ready = true;

    return ERR_OK;
}

error_t thermal_control_step(thermal_controller_t *ctl,
                             const temperature_sample_t *temp,
                             const fan_rpm_sample_t *rpm,
                             uint32_t now_ms,
                             uint8_t *duty_out,
                             fan_alarm_t *alarms_out)
{
    if ((ctl == NULL) || (duty_out == NULL) || (alarms_out == NULL)) {
        return ERR_INVALID_PARAM;
    }
    if (!ctl->ready) {
        return ERR_NOT_INITIALIZED;
    }

    const float dt = thermal_control_dt_s(ctl, now_ms);

    /*
     * ---- 温度有效性（非对称去抖：进得慢、出得也慢）----
     *
     * 进兜底与退出兜底用的**不是同一个阈值**，所以这里必须按当前认定的方向
     * 选 need，不能只传 invalid_debounce_samples：
     *   · 进入（当前不在兜底、要看「连续多少拍无效」）→ invalid_debounce_samples
     *   · 退出（当前在兜底、要看「连续多少拍有效」）  → recovery_debounce_samples
     *
     * 两个方向**共用一个 temp_streak**，因为方向已由 in_temp_fallback 表达：
     * 「与当前认定相反的连续拍数」在进入方向就是连续无效数，在退出方向就是
     * 连续有效数。若两个方向共用同一个阈值，退出就会提前发生，
     * recovery_debounce_samples 也就形同虚设了。
     */
    const bool     temp_valid   = (temp != NULL) && (temp->valid);
    const bool     was_fallback = ctl->in_temp_fallback;
    const uint16_t temp_need    = was_fallback ? ctl->cfg.recovery_debounce_samples
                                               : ctl->cfg.invalid_debounce_samples;

    debounce_step(&ctl->temp_streak, !temp_valid, &ctl->in_temp_fallback, temp_need);

    /*
     * 退出兜底时顺手清掉曲线的迟滞记忆——它在兜底期间没被更新，已经陈旧，
     * 留着会把重入后的第一个目标转速挡住。
     *
     * 挂在「下降沿」上（用调用前的状态判断），而不是嵌在计数器里：
     * 这样无论将来阈值怎么调，清理都只在真正退出兜底的那一拍发生一次。
     */
    if (was_fallback && !ctl->in_temp_fallback) {
        fan_curve_reset(&ctl->curve_state);
    }

    /* ---- 目标转速 ---- */
    uint32_t target_rpm = ctl->cfg.fallback_rpm;

    /*
     * ⚠️ 这里必须同时要求 `temp->valid`，不能只看指针非空。
     *
     * `temperature_from_adc_raw()` 在量程尽头（raw=0 短路 / raw=4095 断路）
     * **仍然会写一个端点温度**到 milli_c 里，同时返回 ERR_UNDERFLOW / OVERFLOW。
     * 若只看指针，那么「NTC 刚被拔掉、去抖还没走完」的那三拍（300 ms）里，
     * 控制器会拿这个端点值去算目标——读到「极热」就冲到满速、读到「极冷」就停转。
     * 既然已经判定这个读数无效，就**不该用它**：那几拍按兜底目标走即可。
     */
    if (!ctl->in_temp_fallback && (temp != NULL) && temp->valid) {
        /*
         * 偏移加在**设定温度**上：调大偏移 = 曲线右移 = 风扇起步更晚 = 更暖。
         * 所以这里是 temp − offset，不是 temp + offset。
         */
        (void)fan_curve_eval(&ctl->curve_state,
                             temp->milli_c - ctl->cfg.temp_offset_milli_c,
                             &target_rpm);
    }

    /*
     * 把目标夹进**物理可达区间**：0 或 [最小可达转速, 满速]。
     *
     * 2026-10-11 上板实测暴露的问题：曲线低端给出 619 rpm 这样的目标，
     * 而空调比下限 20% 对应约 3450 rpm —— 目标永远够不到，PI 只好长期贴在
     * 饱和边界上。现象上风扇转得没错（本来就该是 20%），但「目标 vs 实测」
     * 在屏上和遥测里永远是一对够不着的数，`RPM_NOT_TRACKING` 的语义也说不清。
     *
     * 夹进可达区间之后，PI 的目标始终可达，它就不会长期贴在饱和边上；
     * 这条不变量对**任何**曲线都成立，不必指望每条曲线都画得恰好。
     */
    const uint32_t min_reachable_rpm =
        thermal_min_reachable_rpm(ctl->cfg.duty_min_effective_percent);

    if ((target_rpm > 0U) && (target_rpm < min_reachable_rpm)) {
        target_rpm = min_reachable_rpm;
    }

    /* ---- 转速测量（EMA 抑制 FG 量化噪声） ---- */
    const bool rpm_valid = (rpm != NULL) && (rpm->valid);

    if (rpm_valid) {
        const float alpha = ctl->cfg.rpm_ema_alpha;

        if (!ctl->rpm_filtered_valid || (alpha <= 0.0f) || (alpha >= 1.0f)) {
            /* 第一个样本直接采信，或调用方选择不过滤 */
            ctl->rpm_filtered = (float)rpm->rpm;
        } else {
            ctl->rpm_filtered = (alpha * (float)rpm->rpm)
                              + ((1.0f - alpha) * ctl->rpm_filtered);
        }
        ctl->rpm_filtered_valid = true;
    } else {
        /*
         * 无有效转速期间，滤波值失去意义：它可能是断线**之前**的读数，而这段
         * 时间里风扇的实际转速早变了。不清掉的话，转速恢复后的头几拍会把陈旧值
         * 按 (1−alpha) 的比重混进来喂给 PI。
         * 清掉之后，恢复的第一个有效样本走上面「直接采信」那条分支——正是
         * 「第一个样本」应有的处理。
         */
        ctl->rpm_filtered_valid = false;
    }

    /* ---- 转速丢失（去抖） ----
       两个前提缺一不可，否则会**误报**：

       ① 只有「我们确实要求它转」时，缺反馈才算故障。目标转速为 0 时风扇本就
          该停，没有 FG 沿是**预期行为**——2026-10-11 上板实测踩到：室温下
          目标为 0、风扇停着，系统却一直挂在 DEGRADED + !TACH。 */
    if (target_rpm == 0U) {
        ctl->tach_lost        = false;
        ctl->tach_lost_streak = 0U;
    } else {
        /* ② 只有确定性的「没信号」才算证据（见 tach_evidence_lost）。 */
        const error_t rpm_status = (rpm != NULL) ? rpm->status : ERR_TIMEOUT;
        const bool    missing    = tach_evidence_lost(rpm_valid, rpm_status);

        debounce_step(&ctl->tach_lost_streak, missing, &ctl->tach_lost,
                      ctl->cfg.invalid_debounce_samples);
    }

    /* ---- 控制律 ---- */
    bool  use_pi = false;
    float duty_f = 0.0f;

    if (target_rpm == 0U) {
        /*
         * 目标转速为 0 = 停机档，必须绕开 PI。
         * PI 的输出下限已经是 duty_min（见 thermal_control_init），让它去追
         * 一个 0 的目标，只会把风扇顶在最低速上转——与「停机」自相矛盾。
         */
        ctl->pi_active = false;
        duty_f         = 0.0f;
    } else if (rpm_valid && ctl->rpm_filtered_valid) {
        if (!ctl->pi_active) {
            /* 从开环切回闭环：把积分器预置到当前输出，避免扇叶抽一下。 */
            (void)pi_reset(&ctl->pi_state, (float)ctl->duty_percent);
            ctl->pi_active = true;
        }
        (void)pi_step(&ctl->pi_state, (float)target_rpm, ctl->rpm_filtered, dt, &duty_f);
        use_pi = true;
    } else {
        ctl->pi_active = false;

        /* 内环无反馈 → 开环前馈：按曲线目标反算一个名义占空比。 */
        uint8_t feedforward = 0U;
        (void)fan_control_duty_from_rpm(target_rpm, &feedforward);
        duty_f = (float)feedforward;
    }

    /* ---- 报警 ---- */
    fan_alarm_t alarms = FAN_ALARM_NONE;

    if (ctl->in_temp_fallback) {
        alarms |= FAN_ALARM_TEMP_INVALID;
    }
    if (ctl->tach_lost) {
        alarms |= FAN_ALARM_TACH_LOST;
    }

    if (use_pi && (target_rpm > 0U)) {
        const float error = (float)target_rpm - ctl->rpm_filtered;
        const float tol   = THERMAL_NOT_TRACKING_RATIO * (float)target_rpm;

        if (error > tol) {
            if (ctl->not_tracking_streak < UINT16_MAX) {
                ctl->not_tracking_streak++;
            }
        } else {
            ctl->not_tracking_streak = 0U;
        }

        if (ctl->not_tracking_streak >= (uint16_t)THERMAL_NOT_TRACKING_SAMPLES) {
            alarms |= FAN_ALARM_RPM_NOT_TRACKING;
        }
    } else {
        ctl->not_tracking_streak = 0U;
    }

    /* ---- 落成输出 ---- */
    const uint8_t duty = duty_to_percent(duty_f, ctl->cfg.duty_min_effective_percent);

    ctl->duty_percent = duty;
    ctl->target_rpm   = target_rpm;
    ctl->alarms       = alarms;
    ctl->last_ms      = now_ms;
    ctl->primed       = true;

    *duty_out   = duty;
    *alarms_out = alarms;

    return ERR_OK;
}

error_t thermal_control_set_temp_offset(thermal_controller_t *ctl, int32_t offset_milli_c)
{
    if (ctl == NULL) {
        return ERR_INVALID_PARAM;
    }

    ctl->cfg.temp_offset_milli_c = offset_milli_c;

    /* 曲线迟滞记忆是基于旧偏移建立的，偏移变了它就失去意义。 */
    fan_curve_reset(&ctl->curve_state);

    return ERR_OK;
}

error_t thermal_control_set_gains(thermal_controller_t *ctl, float kp, float ki)
{
    if (ctl == NULL) {
        return ERR_INVALID_PARAM;
    }

    const error_t err = pi_set_gains(&ctl->pi_state, kp, ki);
    if (err != ERR_OK) {
        return err;
    }

    ctl->cfg.pi.kp = kp;
    ctl->cfg.pi.ki = ki;

    return ERR_OK;
}

error_t thermal_control_get_status(const thermal_controller_t *ctl, thermal_status_t *out)
{
    if ((ctl == NULL) || (out == NULL)) {
        return ERR_INVALID_PARAM;
    }
    if (!ctl->ready) {
        return ERR_NOT_INITIALIZED;
    }

    out->target_rpm           = ctl->target_rpm;
    out->rpm_filtered         = ctl->rpm_filtered;
    out->duty_percent         = ctl->duty_percent;
    out->temp_offset_milli_c  = ctl->cfg.temp_offset_milli_c;
    out->alarms               = ctl->alarms;
    out->in_temp_fallback     = ctl->in_temp_fallback;
    out->tach_lost            = ctl->tach_lost;

    /* 「设定温度」取曲线起步点 + 偏移：这是用户直觉里「风扇从几度开始转」。 */
    out->target_temp_milli_c  = ctl->cfg.curve.points[0].temp_milli_c
                              + ctl->cfg.temp_offset_milli_c;

    return ERR_OK;
}
