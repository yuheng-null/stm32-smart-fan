/**
 * @file app_control.c
 * @brief 控制编排的实现。
 *
 * 职责边界见 app_control.h。这里记三处**参数是怎么来的**，因为它们最容易被
 * 后来的人当成「随手填的数」而改坏：
 *
 * ── 曲线锚点 ─────────────────────────────────────────────────────────
 *   横轴（温度）按 NTC 的标称参数换算，表达的是一组合适的室内舒适控制点；
 *   纵轴（转速）用的是与内环同源的**观测单位**。
 *   改这张表不影响算法，也不用动任何别的地方。
 *
 * ── PI 增益 ──────────────────────────────────────────────────────────
 *   由 IMC（λ 整定）从**板上实测辨识**的结果推出来：Kp = τ/(K·λ)、Ki = 1/(K·λ)，
 *   实测 K ≈ 163 rpm/%、τ ≈ 1.4 s。这两个常数不是估计值，来历见下面
 *   APP_PI_KP 上方的整段说明（含第一版按假定 τ 整定、上板即振荡那次事故）。
 *   ⚠️ τ 与死区都是**本风扇、本风道**的实测值，换风扇必须重测后重算。
 *
 * ── 为什么不开输出速率限制 ───────────────────────────────────────────
 *   曲线上的迟滞已经把目标转速的变化限制在每 1 ℃ 几百 rpm 之内，输出不会
 *   阶跃；切模式的冲击由 pi_reset() 的无扰切换负责。而 pi_control 的速率限制
 *   是在**抗饱和判断之后**才施加的，长期生效会让积分器悄悄卷绕——
 *   在没有实际收益的情况下不值得引入这个风险，所以关掉（= 0）。
 *   它仍然是 pi_control 的一个可用能力，留给「设定值可能任意跳变」的场合。
 */

#include "app/app_control.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app/app_display.h"

#include "bsp/bsp_adc.h"
#include "bsp/bsp_buzzer.h"
#include "bsp/bsp_fan.h"
#include "bsp/bsp_key.h"
#include "bsp/bsp_relay.h"
#include "bsp/bsp_system.h"

#include "module/fan_control/fan_control.h"
#include "module/fan_fsm/fan_fsm.h"
#include "module/temperature/temperature.h"
#include "module/thermal_control/thermal_control.h"

/* ----------------------------------------------------------------- 配置 */

/** 温度采样时对 ADC 求平均的次数（抑制工频与开关噪声）。 */
#define APP_CONTROL_ADC_AVERAGES    (16U)

/** 控制周期（毫秒）。OLED 刷新等阻塞都发生在这之间，用真实 dt 补偿。 */
#define APP_CONTROL_PERIOD_MS       (100U)

/*
 * 风扇曲线：温度（毫摄氏度）→ 目标转速（观测单位）。
 *
 * ── 纵轴为什么从 3500 起步而不是 0 ───────────────────────────────────
 *   4 线风扇在 <20% 占空比时行为未定义，而 20% 对应约 3450 观测 rpm
 *   （由 fan_control 的经验模型给出）。所以**实际能用的转速只有
 *   {0} ∪ [≈3450, 满速]**——曲线低端若画成 0→1200→2200，中间那一段是
 *   够不到的（2026-10-11 上板实测暴露：目标 619 rpm 而对风扇最慢就是 3450）。
 *   这里让曲线从「不转」直接跳到「最小可控转速」，中间不留不可达的台阶。
 *   （thermal_control 里另有一道夹取做兜底，任何曲线都不会漏。）
 *
 * ── 这张表的定位 ────────────────────────────────────────────────────
 *   横轴的物理刻度由 NTC 校准决定（步骤见 docs/control.md 的「标定」），
 *   这里表达的是一组合适的室内舒适控制点。运行期可以用串口 TEMP 或按键
 *   整体平移；做完两点校准后按实际温度回填这张表即可。
 *   改这张表不影响任何算法。
 */
static const fan_curve_point_t s_curve_points[] = {
    { 28000,     0U },      /* 28.0 ℃：不转 */
    { 30000,  3500U },      /* 30.0 ℃：直接跳到最小可控转速（20% 对应值） */
    { 35000,  5000U },      /* 35.0 ℃ */
    { 40000,  6500U },      /* 40.0 ℃ */
    { 45000,  8000U },      /* 45.0 ℃ */
    { 50000,  9500U },      /* 50.0 ℃ */
    { 55000, 11500U },      /* 55.0 ℃ */
    { 60000, 13500U },      /* 60.0 ℃ */
    { 70000, 16456U },      /* 70.0 ℃ 及以上：满速（实测 100% 时的观测转速） */
};

#define APP_CURVE_POINT_COUNT   (9U)

/**
 * 内环 PI 增益（IMC 整定，**基于板上实测辨识**）。
 *
 * ── 这两个数是怎么来的（2026-10-11 实测）────────────────────────────
 *   起初按**假定** τ = 0.3 s 整定，上板立刻**持续振荡**：
 *   目标 6005 rpm，实测在 2717 ~ 9868 之间以约 4.5 s 周期摆动。
 *
 *   于是做了正经的阶跃辨识（20% → 60%，方法与原始数据见 docs/control.md）：
 *       · 静态增益 K ≈ 163 rpm/%  （与 README 的拟合一致，稳态误差 1%）
 *       · 时间常数   τ ≈ 1.4 s
 *       · 另有一段约 0.3 s 的**死区**（静摩擦 + 风扇内部限速）
 *
 *   根因很清楚：原来的 Ti = Kp/Ki = 0.30 s，**恰好等于那个错的假设 τ**；
 *   而真实 τ 是 1.4 s —— 积分时间比对象还快近 5 倍，典型的积分致振。
 *
 *   重算时把积分时间钉死在**实测的 τ** 上（IMC 的规矩就是 Ti = τ），
 *   再用 λ 留裕度：取 **λ = 2.5 s**（实测 τ 的约 1.8 倍；原先那版相当于
 *   λ ≈ 0.5 s，激进得多）。λ 的单位是**秒**，代入的是秒而不是 τ 的倍数——
 *   下面这两个数就是按 λ = 2.5 s 算出来的，反解 λ = τ/(K·Kp) ≈ 2.53 s 对得上。
 *       Kp = τ/(K·λ) = 1.4/(163 × 2.5) ≈ 0.0034
 *       Ki = 1/(K·λ) = 1/(163 × 2.5)   ≈ 0.0025   →  Ti = Kp/Ki ≈ 1.4 s = τ ✓
 *
 *   ⚠️ **换风扇、改风道都要重新测 τ 与死区后重算**，不要照抄这两个数。
 */
#define APP_PI_KP                   (0.0034f)
#define APP_PI_KI                   (0.0025f)

/** 温度失效时的兜底目标转速（观测单位）：宁可偏高速，不能停。 */
#define APP_FALLBACK_RPM            (8000U)

/** 有效占空比下限：4 线规范在 20% 以下行为未定义。 */
#define APP_DUTY_MIN_PERCENT        (20U)

/* --------------------------------------------------------------- 状态 */

static thermal_controller_t s_controller;
static fan_fsm_t            s_fsm;
static app_status_t         s_status;
static bool                 s_ready = false;
static bool                 s_oled_present = false;
static int32_t              s_applied_offset = 0;

/**
 * 最近一次由**级联控制器**算出的占空比。
 *
 * 留着它是为了给串口命令路径一个正确的 auto_duty：命令要在**一拍之内**闭环
 * （见 app_control_dispatch_cmd 的说明），而那一刻控制器还没跑，只能拿上一次
 * 的结果。这里必须存控制器自己的输出，**不能拿 s_status.duty_percent 顶替**——
 * 那是最新的**状态机输出**，在 MANUAL 下等于手动档位；用它会让「MANUAL → AUTO」
 * 的第一拍输出旧的百分比，而不是控制器算出来的值。
 */
static uint8_t              s_auto_duty = 0U;

/* ----------------------------------------------------------------- 内部 */

/** @brief 采一次温度并换算。 */
static temperature_sample_t app_control_sample_temperature(uint32_t now_ms)
{
    temperature_sample_t sample = {0};
    uint16_t             raw    = 0U;

    sample.tick_ms = now_ms;

    if (bsp_adc_read_averaged(&raw, (uint16_t)APP_CONTROL_ADC_AVERAGES) != ERR_OK) {
        /* ADC 读失败：当作无效样本，让上层走兜底，而不是拿一个陈旧值凑合。 */
        sample.status = ERR_TIMEOUT;
        return sample;
    }

    sample.adc_raw = raw;
    sample.status  = temperature_from_adc_raw(raw, &sample.milli_c);
    sample.valid   = (sample.status == ERR_OK);

    return sample;
}

/** @brief 采一次转速并换算。 */
static fan_rpm_sample_t app_control_sample_rpm(uint32_t now_ms)
{
    fan_rpm_sample_t sample    = {0};
    uint32_t         period_us = 0U;

    sample.tick_ms = now_ms;

    error_t err = bsp_fan_get_period_us(&period_us);

    if (err == ERR_OK) {
        err = fan_control_rpm_from_period_us(period_us, &sample.rpm);
    }

    sample.status = err;
    sample.valid  = (err == ERR_OK);

    return sample;
}

/**
 * @brief 把按键事件翻译成状态机的值类型事件并立即投递。
 *
 * 每个键一次最多取两个事件（按下 + 松开）：这样一次**快速点按**能在同一个
 * 控制节拍里走完「按下 → 松开」，被正确判成短按，而不会被误当成按下不放。
 */
static void app_control_poll_keys(uint32_t now_ms, uint8_t auto_duty,
                                  bool temp_valid, bool tach_valid)
{
    for (uint8_t k = 0U; k < (uint8_t)KEY_ID_COUNT; k++) {
        for (uint8_t n = 0U; n < 2U; n++) {
            const key_event_t key_ev = bsp_key_take_event((key_id_t)k);

            if (key_ev == KEY_EVENT_NONE) {
                break;
            }

            fan_event_t event = {0};

            event.type    = (key_ev == KEY_EVENT_PRESS) ? FAN_EVENT_KEY_PRESS
                                                        : FAN_EVENT_KEY_RELEASE;
            event.key     = (k == (uint8_t)KEY_ID_K1) ? FAN_KEY_1 : FAN_KEY_2;
            event.tick_ms = now_ms;

            fan_fsm_output_t out;
            (void)fan_fsm_step(&s_fsm, &event, now_ms, auto_duty,
                               temp_valid, tach_valid, &out);
        }
    }
}

/** @brief 把状态机的输出写进硬件。 */
static void app_control_apply(const fan_fsm_output_t *out)
{
    (void)bsp_fan_set_duty_percent(out->duty_percent);
    (void)bsp_relay_set(out->alarm_lamp_on);
    (void)bsp_buzzer_set(out->buzzer_on);
}

/** @brief 更新给显示与遥测用的快照。 */
static void app_control_update_status(const temperature_sample_t *temp,
                                      const fan_rpm_sample_t *rpm,
                                      const thermal_status_t *ctrl,
                                      const fan_fsm_output_t *out)
{
    s_status.mode                 = out->mode;
    s_status.temp_valid           = temp->valid;
    s_status.temp_milli_c         = temp->milli_c;
    s_status.temp_status          = temp->status;
    s_status.adc_raw              = temp->adc_raw;
    s_status.rpm_valid            = rpm->valid;
    s_status.rpm                  = rpm->rpm;
    s_status.duty_percent         = out->duty_percent;
    s_status.target_rpm           = ctrl->target_rpm;
    s_status.target_temp_milli_c  = ctrl->target_temp_milli_c;
    s_status.manual_duty_percent  = out->manual_duty_percent;
    s_status.alarms               = out->alarms;
    s_status.temp_ok              = !ctrl->in_temp_fallback;
    s_status.tach_ok              = !ctrl->tach_lost;
}

/* ----------------------------------------------------------------- 对外 */

error_t app_control_init(bool oled_present)
{
    s_ready        = false;
    s_oled_present = oled_present;

    const thermal_config_t controller_cfg = {
        .curve = {
            .points              = s_curve_points,
            .count               = (uint8_t)APP_CURVE_POINT_COUNT,
            .hysteresis_milli_c  = 1000,            /* ±1.0 ℃，盖住 ADC 噪声 */
            .rpm_at_or_below_min = 0U,
            .rpm_at_or_above_max = 16456U,
        },
        .pi = {
            .kp                     = APP_PI_KP,
            .ki                     = APP_PI_KI,
            .out_min                = 0.0f,
            .out_max                = 100.0f,
            .out_rate_limit_per_s   = 0.0f,         /* 见文件头说明 */
            .allow_integrate_in_sat = false,
        },
        .control_period_ms          = APP_CONTROL_PERIOD_MS,
        .fallback_rpm               = APP_FALLBACK_RPM,
        .invalid_debounce_samples   = 3U,
        .recovery_debounce_samples  = 5U,
        .temp_offset_milli_c        = 0,
        .duty_min_effective_percent = APP_DUTY_MIN_PERCENT,
        .rpm_ema_alpha              = 0.3f,         /* 抑制 FG 量化噪声 */
    };

    error_t err = thermal_control_init(&s_controller, &controller_cfg);
    if (err != ERR_OK) {
        return err;
    }

    const fan_fsm_config_t fsm_cfg = {
        .long_press_ms      = 800U,
        .min_dwell_ms       = 500U,
        .temp_step_milli_c  = 500,      /* K1/K2 每次调 0.5 ℃ */
        .temp_min_milli_c   = -10000,   /* 目标温度可下调 10 ℃（更凉） */
        .temp_max_milli_c   = 15000,    /* 可上调 15 ℃（更暖） */
        .manual_min_percent = APP_DUTY_MIN_PERCENT,
    };

    err = fan_fsm_init(&s_fsm, &fsm_cfg);
    if (err != ERR_OK) {
        return err;
    }

    s_applied_offset = 0;
    s_auto_duty      = 0U;
    s_status = (app_status_t){0};

    s_ready = true;

    return ERR_OK;
}

void app_control_task(void)
{
    if (!s_ready) {
        return;
    }

    const uint32_t now = bsp_system_get_tick_ms();

    const temperature_sample_t temp = app_control_sample_temperature(now);
    const fan_rpm_sample_t     rpm  = app_control_sample_rpm(now);

    /* ---- 级联控制器 ---- */
    uint8_t     auto_duty   = 0U;
    fan_alarm_t ctrl_alarms = FAN_ALARM_NONE;

    (void)thermal_control_step(&s_controller, &temp, &rpm, now, &auto_duty, &ctrl_alarms);

    /* 存给命令路径用（命令到达时控制器还没跑本拍），见 s_auto_duty 的说明。 */
    s_auto_duty = auto_duty;

    thermal_status_t ctrl_status;
    (void)thermal_control_get_status(&s_controller, &ctrl_status);

    /* 这两条是**已经去过抖**的有效性标志，状态机直接拿来用，不重复去抖。 */
    const bool temp_valid = !ctrl_status.in_temp_fallback;
    const bool tach_valid = !ctrl_status.tach_lost;

    /* ---- 外部报警透传（不影响模式迁移，只影响上报） ---- */
    fan_alarm_t external = FAN_ALARM_TEMP_UNCALIBRATED;   /* !CAL：NTC 用标称参数 */

    if (!s_oled_present) {
        external |= FAN_ALARM_OLED_ABSENT;
    }
    if ((ctrl_alarms & FAN_ALARM_RPM_NOT_TRACKING) != 0U) {
        external |= FAN_ALARM_RPM_NOT_TRACKING;
    }
    (void)fan_fsm_set_external_alarms(&s_fsm, external);

    /* ---- 按键 → 状态机 ---- */
    app_control_poll_keys(now, auto_duty, temp_valid, tach_valid);

    /* ---- 状态机推进（无事件的那一拍用来推进长按计时） ---- */
    fan_fsm_output_t fsm_out;
    (void)fan_fsm_step(&s_fsm, NULL, now, auto_duty, temp_valid, tach_valid, &fsm_out);

    /* ---- 落到硬件 ---- */
    app_control_apply(&fsm_out);

    /* ---- 把用户偏移推给控制器 ----
       放在状态机之后，所以偏移会晚一个控制周期（100 ms）生效。
       对温度这种慢对象，100 ms 的滞后完全看不出来，换来的是
       「状态机拥有唯一的偏移真值」这个简单事实。 */
    if (fsm_out.temp_offset_milli_c != s_applied_offset) {
        s_applied_offset = fsm_out.temp_offset_milli_c;
        (void)thermal_control_set_temp_offset(&s_controller, s_applied_offset);
    }

    app_control_update_status(&temp, &rpm, &ctrl_status, &fsm_out);
}

error_t app_control_dispatch_cmd(fan_cmd_t cmd, int32_t value)
{
    if (!s_ready) {
        return ERR_NOT_INITIALIZED;
    }

    fan_event_t event = {0};

    event.type    = FAN_EVENT_CMD;
    event.cmd     = cmd;
    event.value   = value;
    event.tick_ms = bsp_system_get_tick_ms();

    /*
     * 命令一拍内闭环：立刻推状态机、立刻落硬件。
     * 若只入队等下一个控制节拍，串口的 @ACK 回执就会比实际动作早 100 ms，
     * 用它做自动化断言时会读到「还没来得及生效」的状态。
     */
    fan_fsm_output_t out;
    const error_t    err = fan_fsm_step(&s_fsm, &event, event.tick_ms,
                                        s_auto_duty,
                                        s_status.temp_ok, s_status.tach_ok,
                                        &out);

    if (err != ERR_OK) {
        return err;
    }

    app_control_apply(&out);

    if (out.temp_offset_milli_c != s_applied_offset) {
        s_applied_offset = out.temp_offset_milli_c;
        (void)thermal_control_set_temp_offset(&s_controller, s_applied_offset);
    }

    s_status.mode                = out.mode;
    s_status.duty_percent        = out.duty_percent;
    s_status.manual_duty_percent = out.manual_duty_percent;
    s_status.alarms              = out.alarms;

    return ERR_OK;
}

error_t app_control_set_target_temp(int32_t milli_c)
{
    if (!s_ready) {
        return ERR_NOT_INITIALIZED;
    }

    /* 曲线起步点即「风扇开始转的温度」，目标温度相对它取偏移。 */
    const int32_t offset = milli_c - s_curve_points[0].temp_milli_c;

    return app_control_dispatch_cmd(FAN_CMD_TEMP_OFFSET_SET, offset);
}

int32_t app_control_get_target_temp(void)
{
    if (!s_ready) {
        return 0;
    }

    return s_curve_points[0].temp_milli_c + s_applied_offset;
}

error_t app_control_set_gains(float kp, float ki)
{
    if (!s_ready) {
        return ERR_NOT_INITIALIZED;
    }

    return thermal_control_set_gains(&s_controller, kp, ki);
}

bool app_control_get_status(app_status_t *out)
{
    if ((out == NULL) || !s_ready) {
        return false;
    }

    *out = s_status;

    return true;
}
