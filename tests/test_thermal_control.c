/**
 * @file test_thermal_control.c
 * @brief module/thermal_control 的宿主机单元测试。
 *
 * 这是整条业务链里最值得仿真的一层：曲线 + 迟滞 + PI + 兜底 + 传感器去抖，
 * 全都在这里汇合。板子上要把这些跑一遍，需要加热/冷却、拔线、等几十秒；
 * 这里用一个与经验模型一致的一阶对象，几十毫秒就能跑完 300 拍。
 *
 * ── 仿真用的一阶对象 ─────────────────────────────────────────────────
 *     稳态转速 rpm_ss = 163 × duty + 190      （与 fan_control 的经验模型同源）
 *     惯性       rpm ← rpm + (rpm_ss − rpm) · dt/τ ，τ = 1.4 s
 *   τ 取 **2026-10-11 板上阶跃辨识**的实测值（见 docs/control.md §4.2）。
 *   把「对象」与「模型」写成同一个式子是有意的：这样测试验证的是
 *   **控制器结构**（能不能收敛、边界对不对），而不是模型误差。
 */

#include "unity.h"

#include <stdbool.h>
#include <stdint.h>

#include "interface/error.h"
#include "module/common/control_types.h"
#include "module/thermal_control/thermal_control.h"

void setUp(void) {}
void tearDown(void) {}

/* ------------------------------------------------------------ 测试装置 */

/** 曲线：30 ℃ 起步，35 ℃ 对应 5000 rpm，40 ℃ 对应 9000 rpm。 */
static const fan_curve_point_t k_curve_points[] = {
    { 30000, 0U },
    { 35000, 5000U },
    { 40000, 9000U },
};

static thermal_config_t base_config(void)
{
    const thermal_config_t cfg = {
        .curve = {
            .points              = k_curve_points,
            .count               = 3U,
            .hysteresis_milli_c  = 1000,
            .rpm_at_or_below_min = 0U,
            .rpm_at_or_above_max = 9000U,
        },
        .pi = {
            /* 与 app/app_control.c 的发布值一致（基于板上实测辨识整定） */
            .kp                     = 0.0034f,
            .ki                     = 0.0025f,
            .out_min                = 0.0f,
            .out_max                = 100.0f,
            .out_rate_limit_per_s   = 0.0f,
            .allow_integrate_in_sat = false,
        },
        .control_period_ms          = 100U,
        .fallback_rpm               = 8000U,
        /* 与 app/app_control.c 的生产配置一致，且**两个方向故意取不同的值**：
           非对称去抖（进 3 拍、出 5 拍）只有在两者不相等时才测得出。 */
        .invalid_debounce_samples   = 3U,
        .recovery_debounce_samples  = 5U,
        .temp_offset_milli_c        = 0,
        .duty_min_effective_percent = 20U,
        .rpm_ema_alpha              = 1.0f,   /* 不过滤，保证测试确定 */
    };

    return cfg;
}

/**
 * @brief base_config() 的「可取址」版本。
 *
 * C 里不能对函数返回值做强转再取址（`fresh_config()` 非法
 * ——转型结果不是左值）。而多数用例只需要一份默认配置，于是把它固化到一个
 * 文件级实例上，省掉每个用例都写两行。
 */
static thermal_config_t g_default_config;

static thermal_config_t *fresh_config(void)
{
    g_default_config = base_config();
    return &g_default_config;
}

static temperature_sample_t make_temp(int32_t milli_c, bool valid, uint32_t tick_ms)
{
    temperature_sample_t s = {0};

    s.milli_c = milli_c;
    s.adc_raw = 2000U;
    s.status  = valid ? ERR_OK : ERR_UNDERFLOW;
    s.valid   = valid;
    s.tick_ms = tick_ms;

    return s;
}

static fan_rpm_sample_t make_rpm(uint32_t rpm, bool valid, uint32_t tick_ms)
{
    fan_rpm_sample_t s = {0};

    s.rpm     = rpm;
    s.status  = valid ? ERR_OK : ERR_TIMEOUT;
    s.valid   = valid;
    s.tick_ms = tick_ms;

    return s;
}

/**
 * 仿真对象：一阶惯性 + 死区时间，参数取 **2026-10-11 板上阶跃辨识**的结果。
 *
 *   · 静态：rpm_ss = 163 × duty + 190（与经验模型同源）
 *   · τ ≈ 1.4 s
 *   · 等效死区 ≈ 0.8 s（标定到能复现实测振荡，理由见 test_pi_control.c）
 *
 * ⚠️ 死区这一项不能省，也不能取得太小。第一版整定用的模型没有死区，
 *    仿真显示「稳定」，而板上实际持续振荡——**仿真会给出虚假的通过**。
 */
#define PLANT_TAU_S     (1.4f)
#define PLANT_DEAD_S    (0.8f)
#define PLANT_DELAY_MAX (32U)

static float   g_plant_rpm;
static float   g_delay[PLANT_DELAY_MAX];
static uint8_t g_delay_len = 4U;
static uint8_t g_delay_idx;

static void plant_reset(void)
{
    g_plant_rpm = 0.0f;
    g_delay_idx = 0U;
    g_delay_len = (uint8_t)((PLANT_DEAD_S / 0.1f) + 0.5f);

    for (uint8_t i = 0U; i < PLANT_DELAY_MAX; i++) {
        g_delay[i] = 0.0f;
    }
}

static float plant_advance(uint8_t duty, float dt_s)
{
    const float applied = g_delay[g_delay_idx];

    g_delay[g_delay_idx] = (float)duty;
    g_delay_idx = (uint8_t)((g_delay_idx + 1U) % g_delay_len);

    const float steady = (163.0f * applied) + 190.0f;

    g_plant_rpm += (steady - g_plant_rpm) * (dt_s / PLANT_TAU_S);

    return g_plant_rpm;
}

/* ---------------------------------------------------------- 装载与校验 */

static void test_init_rejects_bad_configuration(void)
{
    thermal_controller_t ctl = {0};

    TEST_ASSERT_EQUAL_INT(ERR_INVALID_PARAM, thermal_control_init(NULL, &(thermal_config_t){0}));
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_PARAM, thermal_control_init(&ctl, NULL));

    thermal_config_t cfg = base_config();
    cfg.control_period_ms = 0U;
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_PARAM, thermal_control_init(&ctl, &cfg));

    cfg = base_config();
    cfg.duty_min_effective_percent = 101U;
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_PARAM, thermal_control_init(&ctl, &cfg));

    /* 曲线非法（只有一个点）应被透传出来 */
    cfg = base_config();
    cfg.curve.count = 1U;
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_PARAM, thermal_control_init(&ctl, &cfg));
}

static void test_step_before_init_is_not_initialized(void)
{
    thermal_controller_t ctl = {0};
    uint8_t              duty = 0U;
    fan_alarm_t          alarms = FAN_ALARM_NONE;

    TEST_ASSERT_EQUAL_INT(ERR_NOT_INITIALIZED,
                          thermal_control_step(&ctl, NULL, NULL, 0U, &duty, &alarms));
}

static void test_step_rejects_null_outputs(void)
{
    thermal_controller_t ctl = {0};

    TEST_ASSERT_EQUAL_INT(ERR_OK, thermal_control_init(&ctl, fresh_config()));

    TEST_ASSERT_EQUAL_INT(ERR_INVALID_PARAM,
                          thermal_control_step(&ctl, NULL, NULL, 0U, NULL, NULL));
}

/* -------------------------------------------------------------- 基本行为 */

/** 温度低于曲线起步点 → 目标转速 0 → 停机档，输出必须真是 0。 */
static void test_below_curve_start_stops_the_fan(void)
{
    thermal_controller_t ctl    = {0};
    thermal_status_t     status = {0};
    uint8_t              duty   = 0U;
    fan_alarm_t          alarms = FAN_ALARM_NONE;
    const temperature_sample_t t = make_temp(20000, true, 100U);

    TEST_ASSERT_EQUAL_INT(ERR_OK, thermal_control_init(&ctl, fresh_config()));
    TEST_ASSERT_EQUAL_INT(ERR_OK, thermal_control_step(&ctl, &t, NULL, 100U, &duty, &alarms));

    TEST_ASSERT_EQUAL_UINT8(0U, duty);

    TEST_ASSERT_EQUAL_INT(ERR_OK, thermal_control_get_status(&ctl, &status));
    TEST_ASSERT_EQUAL_UINT32(0U, status.target_rpm);
}

/** 35 ℃ 在曲线上正好对应 5000 rpm。 */
static void test_curve_target_is_used(void)
{
    thermal_controller_t ctl    = {0};
    thermal_status_t     status = {0};
    uint8_t              duty   = 0U;
    fan_alarm_t          alarms = FAN_ALARM_NONE;
    const temperature_sample_t t = make_temp(35000, true, 100U);

    TEST_ASSERT_EQUAL_INT(ERR_OK, thermal_control_init(&ctl, fresh_config()));
    TEST_ASSERT_EQUAL_INT(ERR_OK, thermal_control_step(&ctl, &t, NULL, 100U, &duty, &alarms));
    TEST_ASSERT_EQUAL_INT(ERR_OK, thermal_control_get_status(&ctl, &status));

    TEST_ASSERT_EQUAL_UINT32(5000U, status.target_rpm);
}

/**
 * 闭环仿真：恒温 35 ℃，让控制器驱动仿真对象。
 * 断言收敛到曲线目标，且稳态占空比符合经验模型。
 */
static void test_closed_loop_tracks_curve_target(void)
{
    thermal_controller_t ctl    = {0};
    thermal_status_t     status = {0};
    uint32_t             tick   = 0U;
    uint8_t              duty   = 0U;
    fan_alarm_t          alarms = FAN_ALARM_NONE;

    TEST_ASSERT_EQUAL_INT(ERR_OK, thermal_control_init(&ctl, fresh_config()));
    plant_reset();

    for (int i = 0; i < 300; i++) {
        tick += 100U;

        const temperature_sample_t t = make_temp(35000, true, tick);
        const fan_rpm_sample_t     r = make_rpm((uint32_t)g_plant_rpm, true, tick);

        TEST_ASSERT_EQUAL_INT(ERR_OK, thermal_control_step(&ctl, &t, &r, tick, &duty, &alarms));
        plant_advance(duty, 0.1f);
    }

    TEST_ASSERT_EQUAL_INT(ERR_OK, thermal_control_get_status(&ctl, &status));

    /* 转速贴住曲线目标 5000（占空比按整数百分比量化，会有 ±1 格的极限环，约 ±82 rpm） */
    TEST_ASSERT_FLOAT_WITHIN(250.0f, 5000.0f, g_plant_rpm);

    /* 稳态占空比 ≈ (5000 − 190)/163 ≈ 29.5 */
    TEST_ASSERT_INT_WITHIN(2, 30, (int)duty);
}

/* ---------------------------------------------------------- 占空比下限 */

/**
 * 4 线风扇在 <20% 的行为未定义，所以输出只允许是 0 或 ≥20。
 * 这里让目标落在「20% 才能达到的转速」之下，检查控制器**不会**去试 1~19%。
 */
static void test_duty_never_lands_in_undefined_band(void)
{
    thermal_controller_t ctl    = {0};
    uint32_t             tick   = 0U;
    uint8_t              duty   = 0U;
    fan_alarm_t          alarms = FAN_ALARM_NONE;

    TEST_ASSERT_EQUAL_INT(ERR_OK, thermal_control_init(&ctl, fresh_config()));
    plant_reset();

    /* 33 ℃ → 曲线插值得目标 3000 rpm，低于 20%（=3450 rpm）能达到的速度 */
    for (int i = 0; i < 200; i++) {
        tick += 100U;

        const temperature_sample_t t = make_temp(33000, true, tick);
        const fan_rpm_sample_t     r = make_rpm((uint32_t)g_plant_rpm, true, tick);

        TEST_ASSERT_EQUAL_INT(ERR_OK, thermal_control_step(&ctl, &t, &r, tick, &duty, &alarms));

        TEST_ASSERT_TRUE((duty == 0U) || (duty >= 20U));
        TEST_ASSERT_FALSE((duty > 0U) && (duty < 20U));

        plant_advance(duty, 0.1f);
    }

    /* 够不到目标，控制器应老实停在有效下限上，而不是在 0 与 20 之间来回抽 */
    TEST_ASSERT_EQUAL_UINT8(20U, duty);
}

/**
 * 目标转速必须落在**物理可达区间**：0，或 ≥ 占空比下限对应的转速。
 *
 * 上板实测暴露（2026-10-11）：曲线低端给出 619 rpm，而 4 线风扇最慢就是
 * 20% 对应的约 3450 —— 目标永远够不到，PI 长期贴在饱和边界上。
 * 这条不变量对任何曲线都成立，所以放在控制器里兜底，而不是指望曲线画得恰好。
 */
static void test_target_is_clamped_into_reachable_band(void)
{
    thermal_controller_t ctl    = {0};
    thermal_status_t     status = {0};
    uint8_t              duty   = 0U;
    fan_alarm_t          alarms = FAN_ALARM_NONE;

    TEST_ASSERT_EQUAL_INT(ERR_OK, thermal_control_init(&ctl, fresh_config()));

    /* 31 ℃ 在测试曲线上名义是 1000 rpm，远低于 20% 能达到的转速 */
    const temperature_sample_t t = make_temp(31000, true, 100U);

    TEST_ASSERT_EQUAL_INT(ERR_OK, thermal_control_step(&ctl, &t, NULL, 100U, &duty, &alarms));
    TEST_ASSERT_EQUAL_INT(ERR_OK, thermal_control_get_status(&ctl, &status));

    /* 3450 = 163 × 20% + 190，即 20% 占空比对应的观测转速 */
    TEST_ASSERT_EQUAL_UINT32(3450U, status.target_rpm);
}

/* ---------------------------------------------------------- 温度兜底 */

static void test_single_invalid_temperature_does_not_fallback(void)
{
    thermal_controller_t ctl    = {0};
    thermal_status_t     status = {0};
    uint8_t              duty   = 0U;
    fan_alarm_t          alarms = FAN_ALARM_NONE;
    uint32_t             tick   = 0U;

    TEST_ASSERT_EQUAL_INT(ERR_OK, thermal_control_init(&ctl, fresh_config()));

    /* 先来两次有效 */
    for (int i = 0; i < 2; i++) {
        tick += 100U;
        const temperature_sample_t t = make_temp(35000, true, tick);
        TEST_ASSERT_EQUAL_INT(ERR_OK, thermal_control_step(&ctl, &t, NULL, tick, &duty, &alarms));
    }

    /* 单次无效不该触发兜底（去抖要连续 3 次） */
    tick += 100U;
    const temperature_sample_t bad = make_temp(0, false, tick);
    TEST_ASSERT_EQUAL_INT(ERR_OK, thermal_control_step(&ctl, &bad, NULL, tick, &duty, &alarms));

    TEST_ASSERT_EQUAL_INT(ERR_OK, thermal_control_get_status(&ctl, &status));
    TEST_ASSERT_FALSE(status.in_temp_fallback);
    TEST_ASSERT_EQUAL_UINT32(0U, status.alarms & FAN_ALARM_TEMP_INVALID);
}

/**
 * **无效读数不得参与控制**，哪怕去抖还没走完。
 *
 * `temperature_from_adc_raw()` 在量程尽头（raw=0 短路）时会写一个「极热」的
 * 端点温度到 milli_c 里，同时返回 ERR_UNDERFLOW。若控制器只看指针非空，
 * NTC 刚被拔掉的头三拍就会拿这个端点值去算目标 —— 一头冲到满速。
 *
 * 这条断言把那个 300 ms 的误动作挡住：无效时目标应停在兜底值上。
 */
static void test_invalid_reading_never_steers_the_fan(void)
{
    thermal_controller_t ctl    = {0};
    thermal_status_t     status = {0};
    uint8_t              duty   = 0U;
    fan_alarm_t          alarms = FAN_ALARM_NONE;

    TEST_ASSERT_EQUAL_INT(ERR_OK, thermal_control_init(&ctl, fresh_config()));

    /* valid=false，但 milli_c 是「极热」端点（曲线会给出满速 9000） */
    const temperature_sample_t t = make_temp(200000, false, 100U);

    TEST_ASSERT_EQUAL_INT(ERR_OK, thermal_control_step(&ctl, &t, NULL, 100U, &duty, &alarms));
    TEST_ASSERT_EQUAL_INT(ERR_OK, thermal_control_get_status(&ctl, &status));

    /* 兜底转速 8000，而不是曲线端点给出的 9000 */
    TEST_ASSERT_EQUAL_UINT32(8000U, status.target_rpm);
}

static void test_persistent_invalid_temperature_enters_fallback(void)
{
    thermal_controller_t ctl    = {0};
    thermal_status_t     status = {0};
    uint8_t              duty   = 0U;
    fan_alarm_t          alarms = FAN_ALARM_NONE;
    uint32_t             tick   = 0U;

    TEST_ASSERT_EQUAL_INT(ERR_OK, thermal_control_init(&ctl, fresh_config()));

    for (int i = 0; i < 3; i++) {
        tick += 100U;
        const temperature_sample_t bad = make_temp(0, false, tick);
        TEST_ASSERT_EQUAL_INT(ERR_OK, thermal_control_step(&ctl, &bad, NULL, tick, &duty, &alarms));
    }

    TEST_ASSERT_EQUAL_INT(ERR_OK, thermal_control_get_status(&ctl, &status));

    TEST_ASSERT_TRUE(status.in_temp_fallback);
    TEST_ASSERT_TRUE((status.alarms & FAN_ALARM_TEMP_INVALID) != 0U);
    /* 读不到温度时不能停：目标锁定在安全偏高的兜底转速 */
    TEST_ASSERT_EQUAL_UINT32(8000U, status.target_rpm);
}

static void test_fallback_recovers_after_debounce(void)
{
    thermal_controller_t ctl    = {0};
    thermal_status_t     status = {0};
    uint8_t              duty   = 0U;
    fan_alarm_t          alarms = FAN_ALARM_NONE;
    uint32_t             tick   = 0U;

    TEST_ASSERT_EQUAL_INT(ERR_OK, thermal_control_init(&ctl, fresh_config()));

    for (int i = 0; i < 3; i++) {
        tick += 100U;
        const temperature_sample_t bad = make_temp(0, false, tick);
        TEST_ASSERT_EQUAL_INT(ERR_OK, thermal_control_step(&ctl, &bad, NULL, tick, &duty, &alarms));
    }

    TEST_ASSERT_EQUAL_INT(ERR_OK, thermal_control_get_status(&ctl, &status));
    TEST_ASSERT_TRUE(status.in_temp_fallback);

    /*
     * 退出走的是 recovery_debounce_samples（5 拍），**不是**进入用的那个 3。
     * 这两拍之差就是「非对称」的全部含义：先确认第 3 拍还在兜底里
     * （若退出误用了进入阈值，这里就会提前退出、断言失败），再走满 5 拍。
     */
    for (int i = 0; i < 3; i++) {
        tick += 100U;
        const temperature_sample_t good = make_temp(35000, true, tick);
        TEST_ASSERT_EQUAL_INT(ERR_OK, thermal_control_step(&ctl, &good, NULL, tick, &duty, &alarms));
    }

    TEST_ASSERT_EQUAL_INT(ERR_OK, thermal_control_get_status(&ctl, &status));
    TEST_ASSERT_TRUE_MESSAGE(status.in_temp_fallback,
                             "有效 3 拍后仍在兜底：退出阈值必须是 recovery 的 5 拍");

    /* 走满第 5 拍才退出：报警清零，目标回到曲线值 */
    for (int i = 0; i < 2; i++) {
        tick += 100U;
        const temperature_sample_t good = make_temp(35000, true, tick);
        TEST_ASSERT_EQUAL_INT(ERR_OK, thermal_control_step(&ctl, &good, NULL, tick, &duty, &alarms));
    }

    TEST_ASSERT_EQUAL_INT(ERR_OK, thermal_control_get_status(&ctl, &status));

    TEST_ASSERT_FALSE(status.in_temp_fallback);
    TEST_ASSERT_EQUAL_UINT32(0U, status.alarms & FAN_ALARM_TEMP_INVALID);
    TEST_ASSERT_EQUAL_UINT32(5000U, status.target_rpm);
}

/**
 * 非对称去抖的**另一半**：进兜底要连续 3 拍无效，中间只要有一次有效就重新计数。
 *
 * 这条与上一条合起来才锁住「进得慢、出得也慢」这个行为；单看一条都可能被
 * 一个把两个阈值混用的实现蒙混过去。
 */
static void test_fallback_entry_debounce_restarts_on_a_valid_sample(void)
{
    thermal_controller_t ctl    = {0};
    thermal_status_t     status = {0};
    uint8_t              duty   = 0U;
    fan_alarm_t          alarms = FAN_ALARM_NONE;
    uint32_t             tick   = 0U;

    TEST_ASSERT_EQUAL_INT(ERR_OK, thermal_control_init(&ctl, fresh_config()));

    /* 无效 2 拍 → 有效 1 拍（计数清零）→ 再无效 2 拍：累计无效只有 2 拍，不该进兜底 */
    for (int i = 0; i < 2; i++) {
        tick += 100U;
        const temperature_sample_t bad = make_temp(0, false, tick);
        TEST_ASSERT_EQUAL_INT(ERR_OK, thermal_control_step(&ctl, &bad, NULL, tick, &duty, &alarms));
    }

    tick += 100U;
    const temperature_sample_t good = make_temp(35000, true, tick);
    TEST_ASSERT_EQUAL_INT(ERR_OK, thermal_control_step(&ctl, &good, NULL, tick, &duty, &alarms));

    for (int i = 0; i < 2; i++) {
        tick += 100U;
        const temperature_sample_t bad = make_temp(0, false, tick);
        TEST_ASSERT_EQUAL_INT(ERR_OK, thermal_control_step(&ctl, &bad, NULL, tick, &duty, &alarms));
    }

    TEST_ASSERT_EQUAL_INT(ERR_OK, thermal_control_get_status(&ctl, &status));
    TEST_ASSERT_FALSE_MESSAGE(status.in_temp_fallback,
                              "中间插进一拍有效就该重新计数，2 拍无效不足以进兜底");

    /* 再来 1 拍无效（连续到了 3）才进兜底 */
    tick += 100U;
    const temperature_sample_t bad = make_temp(0, false, tick);
    TEST_ASSERT_EQUAL_INT(ERR_OK, thermal_control_step(&ctl, &bad, NULL, tick, &duty, &alarms));

    TEST_ASSERT_EQUAL_INT(ERR_OK, thermal_control_get_status(&ctl, &status));
    TEST_ASSERT_TRUE(status.in_temp_fallback);
}

/* ---------------------------------------------------------- 转速丢失 */

/**
 * FG 线断了但温度还好：内环失去反馈，应切**开环前馈**按曲线继续给风量，
 * 而不是停转或直接满速。
 */
static void test_tach_lost_falls_back_to_open_loop(void)
{
    thermal_controller_t ctl    = {0};
    thermal_status_t     status = {0};
    uint8_t              duty   = 0U;
    fan_alarm_t          alarms = FAN_ALARM_NONE;
    uint32_t             tick   = 0U;

    TEST_ASSERT_EQUAL_INT(ERR_OK, thermal_control_init(&ctl, fresh_config()));

    for (int i = 0; i < 3; i++) {
        tick += 100U;
        const temperature_sample_t t = make_temp(35000, true, tick);
        TEST_ASSERT_EQUAL_INT(ERR_OK, thermal_control_step(&ctl, &t, NULL, tick, &duty, &alarms));
    }

    TEST_ASSERT_EQUAL_INT(ERR_OK, thermal_control_get_status(&ctl, &status));

    TEST_ASSERT_TRUE(status.tach_lost);
    TEST_ASSERT_TRUE((status.alarms & FAN_ALARM_TACH_LOST) != 0U);

    /* 开环前馈：目标 5000 rpm → (5000 − 190)/163 ≈ 29.5 → 30% */
    TEST_ASSERT_INT_WITHIN(1, 30, (int)duty);
}

/**
 * 上板实测抓到的一个误报（2026-10-11）：
 *
 *   室温下温度低于曲线起点 → 目标转速 0 → 风扇**本就该停** → FG 自然没有沿。
 *   但控制器把「没有沿」当成了丢信号，于是系统一直挂在 DEGRADED + !TACH。
 *
 * 「风扇被命令停转」与「风扇该转却不转」是两件事，必须分开。
 */
static void test_tach_loss_not_reported_when_fan_commanded_off(void)
{
    thermal_controller_t ctl    = {0};
    thermal_status_t     status = {0};
    uint8_t              duty   = 0U;
    fan_alarm_t          alarms = FAN_ALARM_NONE;
    uint32_t             tick   = 0U;

    TEST_ASSERT_EQUAL_INT(ERR_OK, thermal_control_init(&ctl, fresh_config()));

    for (int i = 0; i < 20; i++) {
        tick += 100U;

        const temperature_sample_t t = make_temp(20000, true, tick);   /* 低于曲线起点 */
        const fan_rpm_sample_t     r = make_rpm(0U, false, tick);      /* FG 无信号 */

        TEST_ASSERT_EQUAL_INT(ERR_OK, thermal_control_step(&ctl, &t, &r, tick, &duty, &alarms));
    }

    TEST_ASSERT_EQUAL_INT(ERR_OK, thermal_control_get_status(&ctl, &status));

    TEST_ASSERT_EQUAL_UINT32(0U, status.target_rpm);
    TEST_ASSERT_EQUAL_UINT8(0U, duty);
    TEST_ASSERT_FALSE(status.tach_lost);
    TEST_ASSERT_EQUAL_UINT32(0U, status.alarms & FAN_ALARM_TACH_LOST);
}

/** 反过来：目标转速 > 0 而 FG 超时，必须如实报警。 */
static void test_tach_loss_reported_when_fan_should_be_spinning(void)
{
    thermal_controller_t ctl    = {0};
    thermal_status_t     status = {0};
    uint8_t              duty   = 0U;
    fan_alarm_t          alarms = FAN_ALARM_NONE;
    uint32_t             tick   = 0U;

    TEST_ASSERT_EQUAL_INT(ERR_OK, thermal_control_init(&ctl, fresh_config()));

    for (int i = 0; i < 5; i++) {
        tick += 100U;

        const temperature_sample_t t = make_temp(50000, true, tick);   /* 高温 → 目标 9000 */
        const fan_rpm_sample_t     r = make_rpm(0U, false, tick);      /* ERR_TIMEOUT */

        TEST_ASSERT_EQUAL_INT(ERR_OK, thermal_control_step(&ctl, &t, &r, tick, &duty, &alarms));
    }

    TEST_ASSERT_EQUAL_INT(ERR_OK, thermal_control_get_status(&ctl, &status));

    TEST_ASSERT_GREATER_THAN_UINT32(0U, status.target_rpm);
    TEST_ASSERT_TRUE(status.tach_lost);
    TEST_ASSERT_TRUE((status.alarms & FAN_ALARM_TACH_LOST) != 0U);
}

/**
 * ERR_NOT_READY 是「刚起转、只捕获到一个沿」，是每次风扇启动都会经历的瞬态。
 * 把它也算成丢信号，风扇每次启动都会凭空报一次 DEGRADED。
 */
static void test_not_ready_is_not_treated_as_tach_loss(void)
{
    thermal_controller_t ctl    = {0};
    thermal_status_t     status = {0};
    uint8_t              duty   = 0U;
    fan_alarm_t          alarms = FAN_ALARM_NONE;
    uint32_t             tick   = 0U;

    TEST_ASSERT_EQUAL_INT(ERR_OK, thermal_control_init(&ctl, fresh_config()));

    for (int i = 0; i < 20; i++) {
        tick += 100U;

        const temperature_sample_t t = make_temp(50000, true, tick);
        fan_rpm_sample_t           r = make_rpm(0U, false, tick);

        r.status = ERR_NOT_READY;      /* 还没凑够两个沿 */

        TEST_ASSERT_EQUAL_INT(ERR_OK, thermal_control_step(&ctl, &t, &r, tick, &duty, &alarms));
    }

    TEST_ASSERT_EQUAL_INT(ERR_OK, thermal_control_get_status(&ctl, &status));
    TEST_ASSERT_FALSE(status.tach_lost);
}

/* -------------------------------------------------------------- dt 处理 */

/**
 * 主循环被长时间阻塞（OLED 搬运、调试停顿）之后，一拍不该把输出甩出去。
 * dt 上限把它挡住——这是「实时性有抖动」与「控制不乱」能共存的依据。
 */
static void test_long_stall_does_not_blow_up_output(void)
{
    thermal_controller_t ctl    = {0};
    uint8_t              duty   = 0U;
    fan_alarm_t          alarms = FAN_ALARM_NONE;
    uint32_t             tick   = 0U;

    TEST_ASSERT_EQUAL_INT(ERR_OK, thermal_control_init(&ctl, fresh_config()));
    plant_reset();

    for (int i = 0; i < 300; i++) {
        tick += 100U;
        const temperature_sample_t t = make_temp(35000, true, tick);
        const fan_rpm_sample_t     r = make_rpm((uint32_t)g_plant_rpm, true, tick);

        TEST_ASSERT_EQUAL_INT(ERR_OK, thermal_control_step(&ctl, &t, &r, tick, &duty, &alarms));
        plant_advance(duty, 0.1f);
    }

    const uint8_t steady_duty = duty;

    /* 模拟「主循环被阻塞了 10 秒」：tick 一下子跳过去 */
    tick += 10000U;

    const temperature_sample_t t = make_temp(35000, true, tick);
    const fan_rpm_sample_t     r = make_rpm((uint32_t)g_plant_rpm, true, tick);

    TEST_ASSERT_EQUAL_INT(ERR_OK, thermal_control_step(&ctl, &t, &r, tick, &duty, &alarms));

    TEST_ASSERT_TRUE(duty <= 100U);
    TEST_ASSERT_INT_WITHIN(5, (int)steady_duty, (int)duty);
}

/* -------------------------------------------------------------- 偏移 */

static void test_temp_offset_shifts_operating_point(void)
{
    thermal_controller_t ctl    = {0};
    thermal_status_t     status = {0};
    uint8_t              duty   = 0U;
    fan_alarm_t          alarms = FAN_ALARM_NONE;
    const temperature_sample_t t = make_temp(35000, true, 100U);

    TEST_ASSERT_EQUAL_INT(ERR_OK, thermal_control_init(&ctl, fresh_config()));

    /*
     * 偏移 +2 ℃ → 曲线看到 33 ℃ → 名义目标 3000 rpm；
     * 但 3000 低于占空比下限 20% 对应的约 3450，会被抬到达区间下沿
     * （见下一条用例 test_target_is_clamped_into_reachable_band）。
     */
    TEST_ASSERT_EQUAL_INT(ERR_OK, thermal_control_set_temp_offset(&ctl, 2000));
    TEST_ASSERT_EQUAL_INT(ERR_OK, thermal_control_step(&ctl, &t, NULL, 100U, &duty, &alarms));
    TEST_ASSERT_EQUAL_INT(ERR_OK, thermal_control_get_status(&ctl, &status));
    TEST_ASSERT_EQUAL_UINT32(3450U, status.target_rpm);

    /* 设定温度也应随之显示为 30 ℃ + 2 ℃ */
    TEST_ASSERT_EQUAL_INT32(32000, status.target_temp_milli_c);
}

/* ---------------------------------------------------------- 在线整定 */

static void test_set_gains_updates_config(void)
{
    thermal_controller_t ctl    = {0};

    TEST_ASSERT_EQUAL_INT(ERR_OK, thermal_control_init(&ctl, fresh_config()));

    TEST_ASSERT_EQUAL_INT(ERR_OK, thermal_control_set_gains(&ctl, 0.01f, 0.02f));
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.01f, ctl.cfg.pi.kp);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.02f, ctl.cfg.pi.ki);

    TEST_ASSERT_EQUAL_INT(ERR_INVALID_PARAM, thermal_control_set_gains(&ctl, -1.0f, 0.0f));
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_PARAM, thermal_control_set_gains(NULL, 1.0f, 0.0f));
}

/* ---------------------------------------------------------------- main */

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_init_rejects_bad_configuration);
    RUN_TEST(test_step_before_init_is_not_initialized);
    RUN_TEST(test_step_rejects_null_outputs);

    RUN_TEST(test_below_curve_start_stops_the_fan);
    RUN_TEST(test_curve_target_is_used);
    RUN_TEST(test_closed_loop_tracks_curve_target);

    RUN_TEST(test_duty_never_lands_in_undefined_band);
    RUN_TEST(test_target_is_clamped_into_reachable_band);

    RUN_TEST(test_single_invalid_temperature_does_not_fallback);
    RUN_TEST(test_invalid_reading_never_steers_the_fan);
    RUN_TEST(test_persistent_invalid_temperature_enters_fallback);
    RUN_TEST(test_fallback_recovers_after_debounce);
    RUN_TEST(test_fallback_entry_debounce_restarts_on_a_valid_sample);

    RUN_TEST(test_tach_lost_falls_back_to_open_loop);
    RUN_TEST(test_tach_loss_not_reported_when_fan_commanded_off);
    RUN_TEST(test_tach_loss_reported_when_fan_should_be_spinning);
    RUN_TEST(test_not_ready_is_not_treated_as_tach_loss);
    RUN_TEST(test_long_stall_does_not_blow_up_output);

    RUN_TEST(test_temp_offset_shifts_operating_point);
    RUN_TEST(test_set_gains_updates_config);

    return UNITY_END();
}
