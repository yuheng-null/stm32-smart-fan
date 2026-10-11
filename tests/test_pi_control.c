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

/**
 * 对一阶对象做闭环仿真——本文件里最关键的一条。
 *
 * 假想对象：转速是一阶惯性环节，静态增益 K、时间常数 τ：
 *     d(rpm)/dt = (K·duty − rpm) / τ
 *
 * 用 README 实测的 K ≈ 163 rpm/%（20/50/100% → 3347/8527/16456 的最小二乘拟合），
 * τ 取 0.3 s（**假定值，待实测**），PI 用 IMC 整定推出的初值。
 *
 * 断言的是**收敛性**而不是某一步的具体数值：200 拍（20 s）后转速应贴住目标，
 * 且最后一段时间不再漂移——后者能抓出「参数过于激进导致极限环」这类问题。
 */
static void test_closes_loop_on_first_order_plant(void)
{
    const float plant_gain = 163.0f;    /* rpm / % */
    const float plant_tau  = 0.3f;      /* s */
    const float dt         = 0.1f;
    const float target     = 6000.0f;

    const pi_config_t cfg = {
        .kp                     = 0.0037f,
        .ki                     = 0.0123f,
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

    for (int i = 0; i < 200; i++) {
        TEST_ASSERT_EQUAL_INT(ERR_OK, pi_step(&pi, target, rpm, dt, &duty));
        rpm += (plant_gain * duty - rpm) * (dt / plant_tau);
    }

    /* 收敛：误差在目标的 1% 以内 */
    TEST_ASSERT_FLOAT_WITHIN(target * 0.01f, target, rpm);

    /* 稳态工作点合理：6000 rpm 需要约 6000/163 ≈ 36.8% 的占空比 */
    TEST_ASSERT_FLOAT_WITHIN(5.0f, target / plant_gain, duty);

    /* 不振荡：再跑 50 拍，不应出现极限环（转速保持贴合、输出不再大幅摆动） */
    const float rpm_before = rpm;
    const float duty_before = duty;

    for (int i = 0; i < 50; i++) {
        TEST_ASSERT_EQUAL_INT(ERR_OK, pi_step(&pi, target, rpm, dt, &duty));
        rpm += (plant_gain * duty - rpm) * (dt / plant_tau);
    }

    TEST_ASSERT_FLOAT_WITHIN(target * 0.01f, target, rpm);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, rpm_before, rpm);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, duty_before, duty);
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

    RUN_TEST(test_closes_loop_on_first_order_plant);

    return UNITY_END();
}
