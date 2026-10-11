/**
 * @file test_temperature.c
 * @brief module/temperature 的宿主机单元测试。
 *
 * 这里测的是「一个 12 位 ADC 计数对应多少度」这件**纯数学**的事，
 * 不接板子、不接 NTC。温度模块的三个常数（R25 / B / 分压电阻）按器件的
 * 标称值入场，所以下面的断言刻意分成两类：
 *   · 与常数无关的**结构性**性质（端点、单调方向、错误码）—— 换模块也必须成立；
 *   · 与标称常数绑定的**数值**性质（室温阻值区间）—— 这些在两点校准后要跟着改。
 * 分清楚这两类，才知道哪些断言该动、哪些绝不能动。
 */

#include "unity.h"

#include <stdint.h>

#include "interface/error.h"
#include "module/temperature/temperature.h"

void setUp(void) {}
void tearDown(void) {}

/* ---------------------------------------------------------- 阻值换算 */

/**
 * raw=0 是 AO 被拉到地（短路），raw=4095 是拉到 VCC（断路）。
 * 两者都不是有效读数，阻值必须返回 0（哨兵值），不能返回一个看起来正常的数。
 */
static void test_resistance_endpoints_are_zero(void)
{
    TEST_ASSERT_EQUAL_UINT32(0U, temperature_resistance_from_raw(0U));
    TEST_ASSERT_EQUAL_UINT32(0U, temperature_resistance_from_raw(4095U));
}

/**
 * 室温附近的 raw 应反算出 10 kΩ 量级的阻值。
 * 这个区间由「分压电阻 = 10 kΩ」这个标称值决定，两点校准后需同步更新。
 */
static void test_resistance_room_temp_range(void)
{
    const uint32_t r = temperature_resistance_from_raw(2103U);

    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(9500U, r);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(11500U, r);
}

/**
 * 串联分压的代数性质：raw 越大 → AO 电压越高 → R_NTC 越大，且单调。
 * 这条与任何参数都无关，是硬性质。
 */
static void test_resistance_is_monotonic_in_raw(void)
{
    uint32_t previous = temperature_resistance_from_raw(200U);

    for (uint16_t raw = 400U; raw <= 4000U; raw = (uint16_t)(raw + 200U)) {
        const uint32_t current = temperature_resistance_from_raw(raw);

        TEST_ASSERT_GREATER_THAN_UINT32(previous, current);
        previous = current;
    }
}

/* ---------------------------------------------------------- 温度换算 */

static void test_temperature_rejects_null_output(void)
{
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_PARAM, temperature_from_adc_raw(2103U, NULL));
}

/**
 * 端点不是「错误」，是量程尽头：函数会把输出钳到端点温度并如实报码。
 * 上层据此显示 "---"，而不是把一个看似正常的读数当真。
 */
static void test_temperature_underflow_at_raw_zero(void)
{
    int32_t milli_c = 0;

    TEST_ASSERT_EQUAL_INT(ERR_UNDERFLOW, temperature_from_adc_raw(0U, &milli_c));
}

static void test_temperature_overflow_at_raw_full_scale(void)
{
    int32_t milli_c = 0;

    TEST_ASSERT_EQUAL_INT(ERR_OVERFLOW, temperature_from_adc_raw(4095U, &milli_c));
}

/**
 * 方向性：raw 上升 = AO 上升 = NTC 阻值变大 = **变冷**，温度必须单调下降。
 * 这是本项目实测确认过的方向（README：捏住热敏电阻 raw 下降、T 上升）。
 * 若哪天接反了，这条会立刻变红——比在硬件上盯着屏幕猜快得多。
 */
static void test_temperature_decreases_as_raw_increases(void)
{
    int32_t previous = 0;

    TEST_ASSERT_EQUAL_INT(ERR_OK, temperature_from_adc_raw(500U, &previous));

    for (uint16_t raw = 800U; raw <= 3800U; raw = (uint16_t)(raw + 200U)) {
        int32_t current = 0;

        TEST_ASSERT_EQUAL_INT(ERR_OK, temperature_from_adc_raw(raw, &current));
        TEST_ASSERT_LESS_THAN_INT32(previous, current);
        previous = current;
    }
}

/**
 * 与假定参数绑定的数值点：raw 使 R_NTC ≈ 10 kΩ 时，Beta 方程应给出 ≈ 25.0 ℃。
 * 依据：temperature.h 顶部假定 R0 = 10 kΩ @ 25 ℃。
 * 这条是「参数自洽性」检查——若把 R0 改错，它会报错。
 */
static void test_temperature_at_nominal_resistance_is_25c(void)
{
    int32_t milli_c = 0;

    /* raw = 2048 → R = 10000·2048/(4095−2048) ≈ 10005 Ω */
    TEST_ASSERT_EQUAL_INT(ERR_OK, temperature_from_adc_raw(2048U, &milli_c));

    TEST_ASSERT_INT_WITHIN(200, 25000, milli_c);
}

/* ---------------------------------------------------------------- main */

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_resistance_endpoints_are_zero);
    RUN_TEST(test_resistance_room_temp_range);
    RUN_TEST(test_resistance_is_monotonic_in_raw);
    RUN_TEST(test_temperature_rejects_null_output);
    RUN_TEST(test_temperature_underflow_at_raw_zero);
    RUN_TEST(test_temperature_overflow_at_raw_full_scale);
    RUN_TEST(test_temperature_decreases_as_raw_increases);
    RUN_TEST(test_temperature_at_nominal_resistance_is_25c);

    return UNITY_END();
}
