/**
 * @file test_fan_curve.c
 * @brief module/fan_curve 的宿主机单元测试。
 *
 * 迟滞这种东西在硬件上**极难验证**：你得让温度缓慢爬过换挡点、再爬回来，
 * 盯着屏幕看目标转速有没有在阈值附近抖——一次要几十秒，还看不准。
 * 这里用一条斜率整整齐齐的合成曲线，把「该抖的抖、不该抖的不抖」直接钉死。
 *
 * 测试用的曲线（温度毫摄氏度 → 目标转速）：
 *     20000(20℃) →    0
 *     30000(30℃) → 2000
 *     40000(40℃) → 6000
 * 两段斜率分别是 2000/10℃ 与 4000/10℃，都是整数，便于手算核对。
 */

#include "unity.h"

#include <stdint.h>

#include "interface/error.h"
#include "module/fan_curve/fan_curve.h"

void setUp(void) {}
void tearDown(void) {}

/** 主测试曲线。static const：整机只此一份，points 的生命周期即它的生命周期。 */
static const fan_curve_point_t k_points[] = {
    { 20000, 0U },
    { 30000, 2000U },
    { 40000, 6000U },
};

static const fan_curve_config_t k_config = {
    .points                = k_points,
    .count                 = 3U,
    .hysteresis_milli_c    = 1000,   /* ±1.0 ℃ */
    .rpm_at_or_below_min   = 0U,
    .rpm_at_or_above_max   = 6000U,
};

/** 装载好的曲线实例（每个用例开头重新 init）。 */
static fan_curve_t g_curve;

static void load_default_curve(void)
{
    TEST_ASSERT_EQUAL_INT(ERR_OK, fan_curve_init(&g_curve, &k_config));
}

/* ---------------------------------------------------------- 装载与校验 */

static void test_init_rejects_null(void)
{
    fan_curve_t c = {0};

    TEST_ASSERT_EQUAL_INT(ERR_INVALID_PARAM, fan_curve_init(NULL, &k_config));
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_PARAM, fan_curve_init(&c, NULL));
}

static void test_init_rejects_too_few_points(void)
{
    fan_curve_t            c    = {0};
    fan_curve_config_t     cfg  = k_config;
    cfg.count = 1U;

    TEST_ASSERT_EQUAL_INT(ERR_INVALID_PARAM, fan_curve_init(&c, &cfg));
}

static void test_init_rejects_unsorted_temperatures(void)
{
    fan_curve_t c = {0};
    static const fan_curve_point_t bad[] = {
        { 30000, 0U },
        { 20000, 2000U },      /* 温度倒退 */
    };
    fan_curve_config_t cfg = k_config;

    cfg.points = bad;
    cfg.count  = 2U;

    TEST_ASSERT_EQUAL_INT(ERR_INVALID_PARAM, fan_curve_init(&c, &cfg));
}

static void test_init_rejects_negative_hysteresis(void)
{
    fan_curve_t        c   = {0};
    fan_curve_config_t cfg = k_config;
    cfg.hysteresis_milli_c = -1;

    TEST_ASSERT_EQUAL_INT(ERR_INVALID_PARAM, fan_curve_init(&c, &cfg));
}

/** 零初始化的实例没装过曲线，求值必须明确报「未初始化」，不能返回 0 冒充正常。 */
static void test_eval_before_init_is_not_initialized(void)
{
    fan_curve_t c = {0};
    uint32_t    rpm = 0U;

    TEST_ASSERT_EQUAL_INT(ERR_NOT_INITIALIZED, fan_curve_eval(&c, 30000, &rpm));
}

static void test_eval_rejects_null(void)
{
    uint32_t rpm = 0U;

    load_default_curve();

    TEST_ASSERT_EQUAL_INT(ERR_INVALID_PARAM, fan_curve_eval(NULL, 30000, &rpm));
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_PARAM, fan_curve_eval(&g_curve, 30000, NULL));
}

/* ---------------------------------------------------------- 插值与端点 */

static void test_midpoint_interpolation(void)
{
    uint32_t rpm = 0U;

    load_default_curve();

    /* 25 ℃ 在第一段（20→30 ℃，0→2000）的正中 */
    TEST_ASSERT_EQUAL_INT(ERR_OK, fan_curve_eval(&g_curve, 25000, &rpm));
    TEST_ASSERT_EQUAL_UINT32(1000U, rpm);

    fan_curve_reset(&g_curve);

    /* 35 ℃ 在第二段（30→40 ℃，2000→6000）的正中 */
    TEST_ASSERT_EQUAL_INT(ERR_OK, fan_curve_eval(&g_curve, 35000, &rpm));
    TEST_ASSERT_EQUAL_UINT32(4000U, rpm);
}

static void test_temperature_at_anchor_is_exact(void)
{
    uint32_t rpm = 0U;

    load_default_curve();
    TEST_ASSERT_EQUAL_INT(ERR_OK, fan_curve_eval(&g_curve, 30000, &rpm));
    TEST_ASSERT_EQUAL_UINT32(2000U, rpm);
}

/**
 * 曲线外的两个常量是**独立配置项**，不一定等于首末点的转速。
 * 这里用一条端点转速与配置常量不同的曲线，确认求值走的是配置常量。
 */
static void test_out_of_range_uses_configured_constants(void)
{
    static const fan_curve_point_t pts[] = {
        { 20000, 1000U },
        { 30000, 3000U },
    };
    const fan_curve_config_t cfg = {
        .points              = pts,
        .count               = 2U,
        .hysteresis_milli_c  = 0,
        .rpm_at_or_below_min = 0U,      /* ≠ 首点的 1000 */
        .rpm_at_or_above_max = 9000U,   /* ≠ 末点的 3000 */
    };
    fan_curve_t c   = {0};
    uint32_t    rpm = 0U;

    TEST_ASSERT_EQUAL_INT(ERR_OK, fan_curve_init(&c, &cfg));

    TEST_ASSERT_EQUAL_INT(ERR_OK, fan_curve_eval(&c, 15000, &rpm));
    TEST_ASSERT_EQUAL_UINT32(0U, rpm);

    fan_curve_reset(&c);
    TEST_ASSERT_EQUAL_INT(ERR_OK, fan_curve_eval(&c, 99999, &rpm));
    TEST_ASSERT_EQUAL_UINT32(9000U, rpm);
}

/** 目标转速随温度单调不减——这条与迟滞、与具体参数都无关，是硬性质。 */
static void test_target_rpm_is_monotonic_in_temperature(void)
{
    uint32_t previous = 0U;
    bool     first    = true;

    load_default_curve();

    for (int32_t t = 15000; t <= 45000; t += 1000) {
        uint32_t rpm = 0U;

        TEST_ASSERT_EQUAL_INT(ERR_OK, fan_curve_eval(&g_curve, t, &rpm));

        if (!first) {
            TEST_ASSERT_GREATER_OR_EQUAL_UINT32(previous, rpm);
        }
        previous = rpm;
        first    = false;
    }
}

/* -------------------------------------------------------------- 迟滞 */

/**
 * 核心性质：温度在工作点附近小幅来回，目标转速必须**一动不动**。
 * 若迟滞写错（比如条件写成 >= ref_temp 而不是 >= ref_temp + h），
 * 这条会立刻变红——而在板子上这个抖动只会表现为「转速莫名其妙地喘」。
 */
static void test_hysteresis_suppresses_dither(void)
{
    uint32_t rpm = 0U;

    load_default_curve();

    /* 先在 30 ℃ 定下来：目标 2000 */
    TEST_ASSERT_EQUAL_INT(ERR_OK, fan_curve_eval(&g_curve, 30000, &rpm));
    TEST_ASSERT_EQUAL_UINT32(2000U, rpm);

    /* 在 ±0.4 ℃（小于迟滞带 ±1.0 ℃）内来回扫，输出必须恒定 */
    const int32_t wiggle[] = { 30400, 29600, 30200, 29800, 30000, 30100, 29900 };

    for (unsigned i = 0U; i < (sizeof(wiggle) / sizeof(wiggle[0])); i++) {
        TEST_ASSERT_EQUAL_INT(ERR_OK, fan_curve_eval(&g_curve, wiggle[i], &rpm));
        TEST_ASSERT_EQUAL_UINT32(2000U, rpm);
    }
}

/** 迟滞是「要跨过去才认」，不是「永远不认」：温度真的挪开了，目标必须跟上。 */
static void test_hysteresis_commits_when_temperature_moves_away(void)
{
    uint32_t rpm = 0U;

    load_default_curve();

    TEST_ASSERT_EQUAL_INT(ERR_OK, fan_curve_eval(&g_curve, 30000, &rpm));
    TEST_ASSERT_EQUAL_UINT32(2000U, rpm);

    /* 升到 31.0 ℃：正好等于「参考温度 + 迟滞」，应提交。
       nominal = 2000 + (1000 毫度 / 10000 毫度) × 4000 = 2400
       （第二段是 30→40 ℃ 对应 2000→6000，斜率 4000 每 10 ℃） */
    TEST_ASSERT_EQUAL_INT(ERR_OK, fan_curve_eval(&g_curve, 31000, &rpm));
    TEST_ASSERT_EQUAL_UINT32(2400U, rpm);

    /* 回落到 30.5 ℃：新的参考温度是 31000，要降到 30000 以下才认，所以保持 2400 */
    TEST_ASSERT_EQUAL_INT(ERR_OK, fan_curve_eval(&g_curve, 30500, &rpm));
    TEST_ASSERT_EQUAL_UINT32(2400U, rpm);

    /* 降到 29.9 ℃：跨过 30000 的边界，降档提交。
       nominal = (9900 毫度 / 10000 毫度) × 2000 = 1980 */
    TEST_ASSERT_EQUAL_INT(ERR_OK, fan_curve_eval(&g_curve, 29900, &rpm));
    TEST_ASSERT_EQUAL_UINT32(1980U, rpm);
}

/**
 * reset 丢掉「上次输出」的记忆：复位后第一拍应无条件接受当前温度的名义值，
 * 而不是被旧参考点挡住。
 */
static void test_reset_clears_hysteresis_memory(void)
{
    uint32_t rpm = 0U;

    load_default_curve();

    TEST_ASSERT_EQUAL_INT(ERR_OK, fan_curve_eval(&g_curve, 30000, &rpm));   /* 定在 2000 */
    TEST_ASSERT_EQUAL_UINT32(2000U, rpm);

    /* 29.5 ℃ 的名义值是 1900，但没跨过迟滞带下限（29000），会被挡住 */
    TEST_ASSERT_EQUAL_INT(ERR_OK, fan_curve_eval(&g_curve, 29500, &rpm));
    TEST_ASSERT_EQUAL_UINT32(2000U, rpm);

    /* 复位后同样的温度应立刻被采纳 */
    fan_curve_reset(&g_curve);
    TEST_ASSERT_EQUAL_INT(ERR_OK, fan_curve_eval(&g_curve, 29500, &rpm));
    TEST_ASSERT_EQUAL_UINT32(1900U, rpm);
}

static void test_hysteresis_zero_disables_latching(void)
{
    fan_curve_t        c   = {0};
    fan_curve_config_t cfg = k_config;
    uint32_t           rpm = 0U;

    cfg.hysteresis_milli_c = 0;
    TEST_ASSERT_EQUAL_INT(ERR_OK, fan_curve_init(&c, &cfg));

    TEST_ASSERT_EQUAL_INT(ERR_OK, fan_curve_eval(&c, 30000, &rpm));
    TEST_ASSERT_EQUAL_UINT32(2000U, rpm);

    /* 迟滞为 0 时，任何偏离都应立刻被跟随 */
    TEST_ASSERT_EQUAL_INT(ERR_OK, fan_curve_eval(&c, 29500, &rpm));
    TEST_ASSERT_EQUAL_UINT32(1900U, rpm);

    TEST_ASSERT_EQUAL_INT(ERR_OK, fan_curve_eval(&c, 30000, &rpm));
    TEST_ASSERT_EQUAL_UINT32(2000U, rpm);
}

/* ---------------------------------------------------------------- main */

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_init_rejects_null);
    RUN_TEST(test_init_rejects_too_few_points);
    RUN_TEST(test_init_rejects_unsorted_temperatures);
    RUN_TEST(test_init_rejects_negative_hysteresis);
    RUN_TEST(test_eval_before_init_is_not_initialized);
    RUN_TEST(test_eval_rejects_null);

    RUN_TEST(test_midpoint_interpolation);
    RUN_TEST(test_temperature_at_anchor_is_exact);
    RUN_TEST(test_out_of_range_uses_configured_constants);
    RUN_TEST(test_target_rpm_is_monotonic_in_temperature);

    RUN_TEST(test_hysteresis_suppresses_dither);
    RUN_TEST(test_hysteresis_commits_when_temperature_moves_away);
    RUN_TEST(test_reset_clears_hysteresis_memory);
    RUN_TEST(test_hysteresis_zero_disables_latching);

    return UNITY_END();
}
