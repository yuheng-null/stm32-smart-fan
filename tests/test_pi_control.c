/**
 * @file test_pi_control.c
 * @brief module/pi_control 的宿主机单元测试。
 *
 * 控制器的坑几乎都藏在「边界」里：饱和、速率限制、第一拍、切换模式。
 * 这些在板子上要么复现一次要几十秒（等风扇转起来），要么根本看不出来
 * （积分饱和表现为「该降的时候降不下来」，很容易被当成风扇的机械滞后）。
 * 这里用一个假想的一阶对象做闭环仿真，把收敛性直接变成断言。
 */

#include "unity.h"

#include <stdbool.h>

#include "interface/error.h"
#include "module/pi_control/pi_control.h"

void setUp(void) {}
void tearDown(void) {}

/** 一个「输出不设边界」的基线配置，专供不想被夹取干扰的用例使用。 */
static pi_config_t wide_config(void)
{
    const pi_config_t cfg = {
        .kp                     = 1.0f,
        .ki                     = 0.0f,
        .out_min                = -100000.0f,
        .out_max                = 100000.0f,
        .out_rate_limit_per_s   = 0.0f,
        .allow_integrate_in_sat = false,
    };

    return cfg;
}

/* ---------------------------------------------------------- 装载与校验 */

static void test_init_rejects_bad_arguments(void)
{
    pi_t pi = {0};

    TEST_ASSERT_EQUAL_INT(ERR_INVALID_PARAM, pi_init(NULL, &(pi_config_t){0}));
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_PARAM, pi_init(&pi, NULL));
}

static void test_init_rejects_negative_gains(void)
{
    pi_t               pi  = {0};
    pi_config_t       cfg = wide_config();

    cfg.kp = -1.0f;
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_PARAM, pi_init(&pi, &cfg));

    cfg = wide_config();
    cfg.ki = -1.0f;
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_PARAM, pi_init(&pi, &cfg));
}

static void test_init_rejects_inverted_output_range(void)
{
    pi_t         pi  = {0};
    pi_config_t cfg = wide_config();

    cfg.out_min = 100.0f;
    cfg.out_max = 0.0f;

    TEST_ASSERT_EQUAL_INT(ERR_INVALID_PARAM, pi_init(&pi, &cfg));
}

/* ------------------------------------------------------------ 基本行为 */

static void test_proportional_only(void)
{
    pi_t         pi  = {0};
    pi_config_t cfg = wide_config();
    float        out = 0.0f;

    cfg.kp = 2.0f;
    TEST_ASSERT_EQUAL_INT(ERR_OK, pi_init(&pi, &cfg));
    TEST_ASSERT_EQUAL_INT(ERR_OK, pi_reset(&pi, 0.0f));

    TEST_ASSERT_EQUAL_INT(ERR_OK, pi_step_error(&pi, 50.0f, 0.1f, &out));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 100.0f, out);
}

static void test_integral_accumulates_with_dt(void)
{
    pi_t         pi  = {0};
    pi_config_t cfg = wide_config();
    float        out = 0.0f;

    cfg.kp = 0.0f;
    cfg.ki = 1.0f;
    TEST_ASSERT_EQUAL_INT(ERR_OK, pi_init(&pi, &cfg));
    TEST_ASSERT_EQUAL_INT(ERR_OK, pi_reset(&pi, 0.0f));

    /* 误差 10、dt 0.1 → 积分增量 = 1.0 */
    TEST_ASSERT_EQUAL_INT(ERR_OK, pi_step_error(&pi, 10.0f, 0.1f, &out));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f, out);

    /* 再走一拍 → 2.0 */
    TEST_ASSERT_EQUAL_INT(ERR_OK, pi_step_error(&pi, 10.0f, 0.1f, &out));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 2.0f, out);
}

static void test_output_is_clamped_to_range(void)
{
    pi_t         pi  = {0};
    pi_config_t cfg = wide_config();
    float        out = 0.0f;

    cfg.kp    = 2.0f;
    cfg.out_min = 0.0f;
    cfg.out_max = 100.0f;

    TEST_ASSERT_EQUAL_INT(ERR_OK, pi_init(&pi, &cfg));
    TEST_ASSERT_EQUAL_INT(ERR_OK, pi_reset(&pi, 0.0f));

    TEST_ASSERT_EQUAL_INT(ERR_OK, pi_step_error(&pi, 1000.0f, 0.1f, &out));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 100.0f, out);

    TEST_ASSERT_EQUAL_INT(ERR_OK, pi_step_error(&pi, -1000.0f, 0.1f, &out));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, out);
}

static void test_step_rejects_bad_arguments(void)
{
    pi_t         pi  = {0};
    pi_config_t  cfg = wide_config();
    float        out = 0.0f;

    TEST_ASSERT_EQUAL_INT(ERR_OK, pi_init(&pi, &cfg));

    TEST_ASSERT_EQUAL_INT(ERR_INVALID_PARAM, pi_step_error(NULL, 1.0f, 0.1f, &out));
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_PARAM, pi_step_error(&pi, 1.0f, 0.1f, NULL));
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_PARAM, pi_step_error(&pi, 1.0f, 0.0f, &out));
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_PARAM, pi_step_error(&pi, 1.0f, -0.1f, &out));
}

/* -------------------------------------------------------- 抗积分饱和 */

/**
 * 这是本模块最要紧的一条性质。
 *
 * 场景：误差长期为正、输出被顶在上限（比如风扇已经满速），随后误差**变小但仍为正**。
 *   · 条件积分（allow=false）：积分器在饱和期间没有继续累加，输出应立刻回落到
 *     「比例项 + 已经攒下的那点积分」。
 *   · 不加保护（allow=true）：积分器被顶到上限，输出会**卡在上限下不来**，
 *     这正是「该降的时候降不下来」的经典表现。
 *
 * 两者用同一组增益、同一串输入，只有那一个开关不同——差异完全来自抗饱和。
 */
static void test_anti_windup_freezes_integrator_while_saturated(void)
{
    pi_config_t base = {
        .kp                     = 1.0f,
        .ki                     = 1.0f,
        .out_min                = 0.0f,
        .out_max                = 100.0f,
        .out_rate_limit_per_s   = 0.0f,
        .allow_integrate_in_sat = false,
    };

    pi_t  protected_pi = {0};
    pi_t  naive_pi     = {0};
    float out_protected = 0.0f;
    float out_naive     = 0.0f;

    TEST_ASSERT_EQUAL_INT(ERR_OK, pi_init(&protected_pi, &base));
    TEST_ASSERT_EQUAL_INT(ERR_OK, pi_reset(&protected_pi, 0.0f));

    base.allow_integrate_in_sat = true;
    TEST_ASSERT_EQUAL_INT(ERR_OK, pi_init(&naive_pi, &base));
    TEST_ASSERT_EQUAL_INT(ERR_OK, pi_reset(&naive_pi, 0.0f));

    /* 阶段一：大误差持续 50 拍，两者都会顶到上限 100 */
    for (int i = 0; i < 50; i++) {
        TEST_ASSERT_EQUAL_INT(ERR_OK, pi_step_error(&protected_pi, 1000.0f, 0.1f, &out_protected));
        TEST_ASSERT_EQUAL_INT(ERR_OK, pi_step_error(&naive_pi, 1000.0f, 0.1f, &out_naive));
    }

    TEST_ASSERT_FLOAT_WITHIN(0.001f, 100.0f, out_protected);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 100.0f, out_naive);

    /* 条件积分：积分器应基本没攒下来（第一拍就饱和，之后一直冻结） */
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, pi_get_integrator(&protected_pi));
    /* 不加保护：积分器被顶死在输出上限 */
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 100.0f, pi_get_integrator(&naive_pi));

    /* 阶段二：误差变小但仍为正 —— 输出应立刻回落 */
    TEST_ASSERT_EQUAL_INT(ERR_OK, pi_step_error(&protected_pi, 50.0f, 0.1f, &out_protected));
    TEST_ASSERT_EQUAL_INT(ERR_OK, pi_step_error(&naive_pi, 50.0f, 0.1f, &out_naive));

    /* 受保护的那个：50（比例） + 5（本拍积分） = 55 */
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 55.0f, out_protected);
    /* 没保护的那个：50 + 100(卡住的积分) → 仍被顶在 100 下不来 */
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 100.0f, out_naive);
}

/* ------------------------------------------------------------ 速率限制 */

static void test_rate_limit_bounds_output_change(void)
{
    pi_t         pi  = {0};
    pi_config_t cfg = wide_config();
    float        out = 0.0f;

    cfg.kp                   = 1.0f;
    cfg.ki                   = 0.0f;
    cfg.out_min              = 0.0f;
    cfg.out_max              = 100.0f;
    cfg.out_rate_limit_per_s = 10.0f;   /* dt 0.1 s → 单拍最多变 1.0 */

    TEST_ASSERT_EQUAL_INT(ERR_OK, pi_init(&pi, &cfg));
    TEST_ASSERT_EQUAL_INT(ERR_OK, pi_reset(&pi, 0.0f));

    /* 比例项一下子要到 100，但速率限制只放行 1.0 */
    TEST_ASSERT_EQUAL_INT(ERR_OK, pi_step_error(&pi, 100.0f, 0.1f, &out));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f, out);

    /* 下一拍再走 1.0 → 2.0（斜坡而不是阶跃） */
    TEST_ASSERT_EQUAL_INT(ERR_OK, pi_step_error(&pi, 100.0f, 0.1f, &out));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 2.0f, out);
}

/* -------------------------------------------------------------- 时间步 */

static void test_integral_scales_with_dt(void)
{
    pi_t         pi_a = {0};
    pi_t         pi_b = {0};
    pi_config_t cfg  = wide_config();
    float        out = 0.0f;

    cfg.kp = 0.0f;
    cfg.ki = 1.0f;

    TEST_ASSERT_EQUAL_INT(ERR_OK, pi_init(&pi_a, &cfg));
    TEST_ASSERT_EQUAL_INT(ERR_OK, pi_init(&pi_b, &cfg));
    TEST_ASSERT_EQUAL_INT(ERR_OK, pi_reset(&pi_a, 0.0f));
    TEST_ASSERT_EQUAL_INT(ERR_OK, pi_reset(&pi_b, 0.0f));

    TEST_ASSERT_EQUAL_INT(ERR_OK, pi_step_error(&pi_a, 10.0f, 0.1f, &out));
    const float short_step = out;

    TEST_ASSERT_EQUAL_INT(ERR_OK, pi_step_error(&pi_b, 10.0f, 0.2f, &out));
    const float long_step = out;

    /* 步长翻倍，积分增量也必须翻倍——这正是「dt 要用真实时间」的依据。 */
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 2.0f * short_step, long_step);
}

/* ---------------------------------------------------------- 无扰复位 */

static void test_reset_is_bumpless(void)
{
    pi_t         pi  = {0};
    pi_config_t cfg = wide_config();
    float        out = 0.0f;

    cfg.kp      = 1.0f;
    cfg.out_min = 0.0f;
    cfg.out_max = 100.0f;

    TEST_ASSERT_EQUAL_INT(ERR_OK, pi_init(&pi, &cfg));
    TEST_ASSERT_EQUAL_INT(ERR_OK, pi_reset(&pi, 42.0f));

    /* 误差为 0 时，输出应继续停在预置值上，不跳变。 */
    TEST_ASSERT_EQUAL_INT(ERR_OK, pi_step_error(&pi, 0.0f, 0.1f, &out));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 42.0f, out);
}

static void test_reset_clamps_output_into_range(void)
{
    pi_t         pi  = {0};
    pi_config_t cfg = wide_config();
    float        out = 0.0f;

    cfg.out_min = 0.0f;
    cfg.out_max = 100.0f;

    TEST_ASSERT_EQUAL_INT(ERR_OK, pi_init(&pi, &cfg));
    TEST_ASSERT_EQUAL_INT(ERR_OK, pi_reset(&pi, 999.0f));

    TEST_ASSERT_EQUAL_INT(ERR_OK, pi_step_error(&pi, 0.0f, 0.1f, &out));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 100.0f, out);
}

/* ------------------------------------------------------- 在线整定 */

static void test_set_gains_keeps_state(void)
{
    pi_t         pi  = {0};
    pi_config_t cfg = wide_config();
    float        out = 0.0f;

    cfg.kp = 0.0f;
    cfg.ki = 1.0f;

    TEST_ASSERT_EQUAL_INT(ERR_OK, pi_init(&pi, &cfg));
    TEST_ASSERT_EQUAL_INT(ERR_OK, pi_reset(&pi, 0.0f));
    TEST_ASSERT_EQUAL_INT(ERR_OK, pi_step_error(&pi, 10.0f, 0.1f, &out));   /* 积分器 = 1 */

    TEST_ASSERT_EQUAL_INT(ERR_OK, pi_set_gains(&pi, 0.5f, 1.0f));

    /* 只换增益：积分器不该被清掉（清了的话调参就变成重启，看不到连续趋势） */
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f, pi_get_integrator(&pi));

    TEST_ASSERT_EQUAL_INT(ERR_OK, pi_step_error(&pi, 10.0f, 0.1f, &out));
    /* 0.5×10 + (1 + 1×10×0.1) = 5 + 2 = 7 */
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 7.0f, out);

    TEST_ASSERT_EQUAL_INT(ERR_INVALID_PARAM, pi_set_gains(&pi, -1.0f, 0.0f));
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_PARAM, pi_set_gains(NULL, 1.0f, 0.0f));
}

/* ------------------------------------------------------- 闭环仿真 */

/*
 * ── 仿真对象：一阶惯性 + 死区时间 + 占空比整数化 ─────────────────────
 *
 * 这几个特征**都是板上实测到的**，缺一个这个仿真就抓不到真实的整定错误：
 *   · 静态增益 K ≈ 163 rpm/%、截距 190 rpm（README 实测最小二乘拟合）
 *   · 时间常数 τ ≈ 1.4 s      （2026-10-11 阶跃辨识 20%→60%）
 *   · 死区约 0.3 s            （阶跃响应起步那段平坦）
 *   · 固件把占空比落成**整数百分比**，1% = 163 rpm，这是个很粗的执行器
 *
 * 稳态式与 module/fan_control/fan_control.h 的经验模型**逐字同源**
 * （rpm = 163×duty + 190），截距不能省：省掉它，对象的稳态工作点就比真实的
 * 偏低 190/163 ≈ 1.2% 占空比，那样算出来的稳态值与断言容差之间的关系会失真。
 *
 * 为什么必须带死区：本工程第一版按假定的 τ=0.3 s 整定，板上**持续振荡**
 * （2717~9868 rpm，周期约 4.5 s）。用不带死区的模型仿真时它却是稳定的——
 * 也就是说，**没有死区的仿真会给出虚假的通过**。这条用例的意义就在这里。
 */
#define PLANT_GAIN      (163.0f)    /* rpm / % */
#define PLANT_INTERCEPT (190.0f)    /* rpm，经验模型的截距 */
#define PLANT_TAU_S     (1.4f)      /* s */

/*
 * 第一版的发布增益（按**假定** τ = 0.3 s 整定，见 docs/control.md §4.1）。
 * 保留在这里是为了给「按错的 τ 整定 → 上板振荡」这个结论留一条回归护栏：
 * 下面 test_first_version_gains_... 会验证它在同一个仿真里**必须**振起来。
 */
#define V1_KP           (0.0037f)
#define V1_KI           (0.0123f)

/*
 * 等效死区。
 *
 * 阶跃响应上肉眼可见的平坦段只有约 0.3 s，但**光有 0.3 s 复现不出板上的振荡**：
 * 第一版增益在这种模型下仿真通过，上板却持续振。说明真实环路里的滞后比
 * 「纯延时」更多——量测通路（FG 周期 + EMA）、风扇内部的二阶动态，都被这里
 * 这个参数一并吸收了。
 *
 * 取 0.8 s 的依据是**标定到能复现实测现象**：这个值下，第一版增益仿真出
 * 持续极限环（与板上一致），而现在的增益稳定。测试的价值就在这条区分度上——
 * 死区取得太小，测试会给出虚假的通过。
 */
#define PLANT_DEAD_S    (0.8f)
#define PLANT_DELAY_MAX (32U)

static float   g_delay[PLANT_DELAY_MAX];
static uint8_t g_delay_len;
static uint8_t g_delay_idx;

static void plant_reset(uint8_t delay_samples)
{
    for (uint8_t i = 0U; i < PLANT_DELAY_MAX; i++) {
        g_delay[i] = 0.0f;
    }
    g_delay_len = (delay_samples == 0U) ? 1U : delay_samples;
    g_delay_idx = 0U;
}

/** @brief 推进一步对象：占空比经死区延时后作用到一阶惯性环节上。 */
static float plant_advance(float duty, float rpm, float dt_s)
{
    const float applied = g_delay[g_delay_idx];

    /* 占空比在固件里是整数百分比，仿真的输入也照此量化 */
    g_delay[g_delay_idx] = (float)((int)(duty + 0.5f));
    g_delay_idx = (uint8_t)((g_delay_idx + 1U) % g_delay_len);

    const float steady = (PLANT_GAIN * applied) + PLANT_INTERCEPT;

    return rpm + ((steady - rpm) * (dt_s / PLANT_TAU_S));
}

/**
 * 在**实测辨识出来的对象**上跑一次闭环，回报三个观测量。
 *
 * 跑 40 s 让环路稳定，再跑 5 s 量末段的峰峰波动。三条断言（收敛、稳态占空比、
 * 没有极限环）共用这一段仿真，所以抽成函数——两处（新版增益 / 第一版增益）
 * 必须是**同一个对象、同一段时长**，否则「新版稳、旧版振」这个对照就不成立。
 *
 * @param kp, ki     被测增益。
 * @param target     设定转速。
 * @param final_rpm  输出，40 s 时的转速。
 * @param final_duty 输出，40 s 时的占空比（整数百分比）。
 * @param tail_pk_pk 输出，末段 5 s 的峰峰波动。
 */
static void simulate_closed_loop(float kp, float ki, float target,
                                 float *final_rpm, float *final_duty, float *tail_pk_pk)
{
    const float   dt     = 0.1f;
    const uint8_t dead_n = (uint8_t)((PLANT_DEAD_S / dt) + 0.5f);

    const pi_config_t cfg = {
        .kp                     = kp,
        .ki                     = ki,
        .out_min                = 0.0f,
        .out_max                = 100.0f,
        .out_rate_limit_per_s   = 0.0f,
        .allow_integrate_in_sat = false,
    };

    pi_t  pi   = {0};
    float duty = 0.0f;
    float rpm  = 0.0f;

    TEST_ASSERT_EQUAL_INT(ERR_OK, pi_init(&pi, &cfg));
    TEST_ASSERT_EQUAL_INT(ERR_OK, pi_reset(&pi, 0.0f));
    plant_reset(dead_n);

    for (int i = 0; i < 400; i++) {          /* 40 s */
        TEST_ASSERT_EQUAL_INT(ERR_OK, pi_step(&pi, target, rpm, dt, &duty));
        rpm = plant_advance(duty, rpm, dt);
    }

    *final_rpm  = rpm;
    *final_duty = duty;

    float lo = 1.0e9f;
    float hi = -1.0e9f;

    for (int i = 0; i < 50; i++) {           /* 再 5 s，量末段波动 */
        TEST_ASSERT_EQUAL_INT(ERR_OK, pi_step(&pi, target, rpm, dt, &duty));
        rpm = plant_advance(duty, rpm, dt);

        if (rpm < lo) { lo = rpm; }
        if (rpm > hi) { hi = rpm; }
    }

    *tail_pk_pk = hi - lo;
}

/**
 * 发布增益在实测辨识出来的对象上必须**收敛且不振荡**。
 *
 * 增益取的是 app/app_control.c 里的发布值（IMC，λ = 2.5 s）。
 * 断言的是结构性质而不是某一步的具体数值：
 *   ① 40 s 后转速贴住目标；
 *   ② 稳态工作点合理（6000 rpm 需要约 (6000−190)/163 ≈ 35.6% 占空比）；
 *   ③ 末段 5 s 的峰峰波动在一个量化台阶的量级（抓「参数过于激进导致极限环」）。
 */
static void test_closes_loop_on_identified_plant(void)
{
    const float target = 6000.0f;

    float rpm     = 0.0f;
    float duty    = 0.0f;
    float pk_pk   = 0.0f;

    simulate_closed_loop(0.0034f, 0.0025f, target, &rpm, &duty, &pk_pk);

    TEST_ASSERT_FLOAT_WITHIN(target * 0.02f, target, rpm);
    TEST_ASSERT_FLOAT_WITHIN(4.0f, (target - PLANT_INTERCEPT) / PLANT_GAIN, duty);

    /* 容许的峰峰波动就是 1% 占空比的量化台阶（163 rpm），留一倍余量。 */
    TEST_ASSERT_FLOAT_WITHIN(320.0f, 0.0f, pk_pk);
}

/**
 * ⭐ **第一版的增益必须在这个仿真里重新振起来。**
 *
 * 这是全项目最要紧的一条回归护栏。docs/control.md §4.1 记的那次事故是：
 * 按**假定** τ = 0.3 s 整定出的 Kp=0.0037 / Ki=0.0123，上板立刻持续振荡
 * （2717~9868 rpm，周期约 4.5 s），根因是积分时间恰好等于那个错的 τ。
 *
 * 为什么必须把「旧增益会振」也断言下来，而不是只断言「新增益稳定」：
 * 只测后者的话，任何人把这个仿真的参数（τ、死区）调松，两条断言照样通过，
 * 而 §4.1 那个结论就失去了可复现性。**有区分度的测试才是有价值的测试**——
 * 这条用例专门负责证明「这个仿真确实能分辨好坏整定」。
 *
 * 判据取「末段峰峰 > 1000 rpm」：远超 1% 占空比的量化台阶（163 rpm），
 * 与实测的 ±3500 rpm 极限环同量级而留足余量，不会因编译器的浮点细节而翻。
 */
static void test_first_version_gains_reproduce_the_board_limit_cycle(void)
{
    float rpm   = 0.0f;
    float duty  = 0.0f;
    float pk_pk = 0.0f;

    simulate_closed_loop(V1_KP, V1_KI, 6000.0f, &rpm, &duty, &pk_pk);

    TEST_ASSERT_TRUE_MESSAGE(pk_pk > 1000.0f,
                             "第一版增益(按假定 tau 整定)必须在该对象上再现极限环；"
                             "若这里是绿的，说明仿真已经失去区分度");
}

/* ---------------------------------------------------------------- main */

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_init_rejects_bad_arguments);
    RUN_TEST(test_init_rejects_negative_gains);
    RUN_TEST(test_init_rejects_inverted_output_range);

    RUN_TEST(test_proportional_only);
    RUN_TEST(test_integral_accumulates_with_dt);
    RUN_TEST(test_output_is_clamped_to_range);
    RUN_TEST(test_step_rejects_bad_arguments);

    RUN_TEST(test_anti_windup_freezes_integrator_while_saturated);
    RUN_TEST(test_rate_limit_bounds_output_change);
    RUN_TEST(test_integral_scales_with_dt);

    RUN_TEST(test_reset_is_bumpless);
    RUN_TEST(test_reset_clamps_output_into_range);
    RUN_TEST(test_set_gains_keeps_state);

    RUN_TEST(test_closes_loop_on_identified_plant);
    RUN_TEST(test_first_version_gains_reproduce_the_board_limit_cycle);

    return UNITY_END();
}
