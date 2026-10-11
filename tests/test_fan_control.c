/**
 * @file test_fan_control.c
 * @brief module/fan_control 的宿主机单元测试。
 *
 * 覆盖两件纯计算：FG 周期 → 转速的换算，以及占空比档位的加减与夹取。
 * 两者都不碰硬件，所以这里能把硬件上很难凑出来的边界（period=0、毛刺、
 * 恰好等于上限、越界入参）逐一喂进去。
 */

#include "unity.h"

#include <stdint.h>

#include "interface/error.h"
#include "module/fan_control/fan_control.h"

void setUp(void) {}
void tearDown(void) {}

/* ---------------------------------------------------------- 转速换算 */

/*
 * 换算依据（见 fan_control.h 顶部）：TIM2 配成 1 计数 = 1 µs，
 * Intel 4 线规范 FG 每转 2 个脉冲，故 RPM = 60000000 / (period_us × 2)
 *                                       = 30000000 / period_us。
 */

static void test_rpm_known_period(void)
{
    uint32_t rpm = 0U;

    /* 半转 30 ms → 1000 RPM */
    TEST_ASSERT_EQUAL_INT(ERR_OK, fan_control_rpm_from_period_us(30000U, &rpm));
    TEST_ASSERT_EQUAL_UINT32(1000U, rpm);
}

static void test_rpm_rejects_null(void)
{
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_PARAM, fan_control_rpm_from_period_us(30000U, NULL));
}

/**
 * period=0 是「两次捕获读到同一个计数」——对风扇而言不可能（那会是 30e6 RPM），
 * 只可能是毛刺。必须拒绝，而不是算出一个天文数字。
 */
static void test_rpm_rejects_zero_period(void)
{
    uint32_t rpm = 0U;

    TEST_ASSERT_EQUAL_INT(ERR_INVALID_PARAM, fan_control_rpm_from_period_us(0U, &rpm));
}

/** 恰好等于上限：30000000 / 1000 = 30000 == FAN_CONTROL_RPM_MAX，是合法读数。 */
static void test_rpm_at_max_is_accepted(void)
{
    uint32_t rpm = 0U;

    TEST_ASSERT_EQUAL_INT(ERR_OK, fan_control_rpm_from_period_us(1000U, &rpm));
    TEST_ASSERT_EQUAL_UINT32(FAN_CONTROL_RPM_MAX, rpm);
}

/** 再快一点就超过上限 → 判为毛刺。 */
static void test_rpm_above_max_is_overflow(void)
{
    uint32_t rpm = 0U;

    TEST_ASSERT_EQUAL_INT(ERR_OVERFLOW, fan_control_rpm_from_period_us(999U, &rpm));
}

/** 间隔长到算出的转速不足 1 RPM → 按停转处理（UNDERFLOW），不是「转速为 0 的正常值」。 */
static void test_rpm_too_slow_is_underflow(void)
{
    uint32_t rpm = 0U;

    TEST_ASSERT_EQUAL_INT(ERR_UNDERFLOW, fan_control_rpm_from_period_us(30000001U, &rpm));
}

/* ---------------------------------------------------------- 档位步进 */

static void test_duty_step_increases_by_step(void)
{
    TEST_ASSERT_EQUAL_UINT8(10U, fan_control_duty_step(0U, FAN_DUTY_UP));
    TEST_ASSERT_EQUAL_UINT8(50U, fan_control_duty_step(40U, FAN_DUTY_UP));
}

static void test_duty_step_decreases_by_step(void)
{
    TEST_ASSERT_EQUAL_UINT8(0U, fan_control_duty_step(10U, FAN_DUTY_DOWN));
    TEST_ASSERT_EQUAL_UINT8(30U, fan_control_duty_step(40U, FAN_DUTY_DOWN));
}

static void test_duty_step_clamps_at_upper_bound(void)
{
    /* 90 + 10 = 100 正好到顶 */
    TEST_ASSERT_EQUAL_UINT8(FAN_CONTROL_DUTY_MAX_PERCENT,
                            fan_control_duty_step(90U, FAN_DUTY_UP));
    /* 95 距上限不足一步 → 直接落到 100，不做部分步进 */
    TEST_ASSERT_EQUAL_UINT8(FAN_CONTROL_DUTY_MAX_PERCENT,
                            fan_control_duty_step(95U, FAN_DUTY_UP));
    /* 已在顶 */
    TEST_ASSERT_EQUAL_UINT8(FAN_CONTROL_DUTY_MAX_PERCENT,
                            fan_control_duty_step(100U, FAN_DUTY_UP));
}

static void test_duty_step_clamps_at_lower_bound(void)
{
    /* 10 - 10 = 0 正好到底 */
    TEST_ASSERT_EQUAL_UINT8(FAN_CONTROL_DUTY_MIN_PERCENT,
                            fan_control_duty_step(10U, FAN_DUTY_DOWN));
    /* 5 距下限不足一步 → 直接落到 0 */
    TEST_ASSERT_EQUAL_UINT8(FAN_CONTROL_DUTY_MIN_PERCENT,
                            fan_control_duty_step(5U, FAN_DUTY_DOWN));
    /* 已在底 */
    TEST_ASSERT_EQUAL_UINT8(FAN_CONTROL_DUTY_MIN_PERCENT,
                            fan_control_duty_step(0U, FAN_DUTY_DOWN));
}

/**
 * 越界入参要先被夹进合法区间再运算——否则 duty+STEP 在 250 附近会回绕成小值。
 * 这是串口/按键之外的调用方（未来从别处解析数值）最容易踩的一脚。
 */
static void test_duty_step_clamps_illegal_input_first(void)
{
    TEST_ASSERT_EQUAL_UINT8(90U, fan_control_duty_step(200U, FAN_DUTY_DOWN));
    TEST_ASSERT_EQUAL_UINT8(FAN_CONTROL_DUTY_MAX_PERCENT,
                            fan_control_duty_step(200U, FAN_DUTY_UP));
}

/* ------------------------------------------------------- 经验模型反解 */

/**
 * 反解要与实测点对得上：README 实测 50% → 8527 rpm，
 * 反解回去应当仍在 50% ±2 以内（拟合本身有约 2% 偏差）。
 */
static void test_duty_from_rpm_roundtrip(void)
{
    uint8_t duty = 0U;

    TEST_ASSERT_EQUAL_INT(ERR_OK, fan_control_duty_from_rpm(8527U, &duty));
    TEST_ASSERT_INT_WITHIN(2, 50, (int)duty);

    TEST_ASSERT_EQUAL_INT(ERR_OK, fan_control_duty_from_rpm(16456U, &duty));
    TEST_ASSERT_INT_WITHIN(2, 100, (int)duty);
}

static void test_duty_from_rpm_rejects_null(void)
{
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_PARAM, fan_control_duty_from_rpm(5000U, NULL));
}

/** 低于模型有效范围（≤ 截距）→ 按停机处理，输出 0，并如实报码。 */
static void test_duty_from_rpm_below_model_range(void)
{
    uint8_t duty = 99U;

    TEST_ASSERT_EQUAL_INT(ERR_UNDERFLOW, fan_control_duty_from_rpm(0U, &duty));
    TEST_ASSERT_EQUAL_UINT8(0U, duty);

    duty = 99U;
    TEST_ASSERT_EQUAL_INT(ERR_UNDERFLOW,
                          fan_control_duty_from_rpm(FAN_CONTROL_RPM_INTERCEPT, &duty));
    TEST_ASSERT_EQUAL_UINT8(0U, duty);
}

/** 目标转速高到模型之外 → 夹到 100%，而不是回绕。 */
static void test_duty_from_rpm_clamps_above_model_range(void)
{
    uint8_t duty = 0U;

    TEST_ASSERT_EQUAL_INT(ERR_OK, fan_control_duty_from_rpm(1000000U, &duty));
    TEST_ASSERT_EQUAL_UINT8(FAN_CONTROL_DUTY_MAX_PERCENT, duty);
}

/* ---------------------------------------------------------------- main */

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_rpm_known_period);
    RUN_TEST(test_rpm_rejects_null);
    RUN_TEST(test_rpm_rejects_zero_period);
    RUN_TEST(test_rpm_at_max_is_accepted);
    RUN_TEST(test_rpm_above_max_is_overflow);
    RUN_TEST(test_rpm_too_slow_is_underflow);

    RUN_TEST(test_duty_step_increases_by_step);
    RUN_TEST(test_duty_step_decreases_by_step);
    RUN_TEST(test_duty_step_clamps_at_upper_bound);
    RUN_TEST(test_duty_step_clamps_at_lower_bound);
    RUN_TEST(test_duty_step_clamps_illegal_input_first);

    RUN_TEST(test_duty_from_rpm_roundtrip);
    RUN_TEST(test_duty_from_rpm_rejects_null);
    RUN_TEST(test_duty_from_rpm_below_model_range);
    RUN_TEST(test_duty_from_rpm_clamps_above_model_range);

    return UNITY_END();
}
