/**
 * @file test_display_view.c
 * @brief module/display_view 的宿主机单元测试。
 *
 * 这里最要紧的一条是**行宽**：6×8 字库在 128 像素宽下每行只能放 21 个字符，
 * 而 ssd1306_write_string() 超宽时**不报错**，只是把多余的字画到看不见的地方。
 * 本工程早先踩过这个坑（README 有记录）。把约束变成断言之后，
 * 以后改文案若撑破了行宽，CI 会立刻拦下来，而不是等谁在硬件上肉眼发现「字没了」。
 */

#include "unity.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "interface/error.h"
#include "module/display_view/display_view.h"

void setUp(void) {}
void tearDown(void) {}

static display_view_input_t make_input(void)
{
    display_view_input_t in = {0};

    in.mode                 = FAN_MODE_AUTO;
    in.temp_valid           = true;
    in.temp_milli_c         = 23800;    /* 23.8 ℃ */
    in.rpm_valid            = true;
    in.rpm                  = 5980U;
    in.duty_percent         = 42U;
    in.target_rpm           = 6000U;
    in.target_temp_milli_c  = 30000U;
    in.manual_duty_percent  = 50U;
    in.alarms               = FAN_ALARM_NONE;

    return in;
}

/** 每一行都必须放得进屏幕——这是本模块存在的首要理由。 */
static void check_all_lines_fit(const display_view_t *view)
{
    TEST_ASSERT_LESS_OR_EQUAL_UINT8(DISPLAY_VIEW_MAX_LINES, view->line_count);

    for (uint8_t i = 0U; i < view->line_count; i++) {
        TEST_ASSERT_LESS_OR_EQUAL_UINT32(21U, (uint32_t)strlen(view->lines[i]));
    }
}

/* ------------------------------------------------------------ 基本内容 */

static void test_builds_temperature_and_mode(void)
{
    const display_view_input_t in = make_input();
    display_view_t             view;

    TEST_ASSERT_EQUAL_INT(ERR_OK, display_view_build(&in, &view));

    TEST_ASSERT_EQUAL_STRING("23.8", view.temp_text);
    TEST_ASSERT_EQUAL_STRING("AUTO", view.mode_text);
    check_all_lines_fit(&view);
}

/** 温度无效时用占位符，绝不显示一个看似正常的数。 */
static void test_invalid_temperature_shows_placeholder(void)
{
    display_view_input_t in = make_input();
    display_view_t       view;

    in.temp_valid = false;

    TEST_ASSERT_EQUAL_INT(ERR_OK, display_view_build(&in, &view));
    TEST_ASSERT_EQUAL_STRING("---", view.temp_text);
    check_all_lines_fit(&view);
}

static void test_auto_mode_shows_target_temperature(void)
{
    const display_view_input_t in = make_input();
    display_view_t             view;

    TEST_ASSERT_EQUAL_INT(ERR_OK, display_view_build(&in, &view));

    TEST_ASSERT_TRUE(strstr(view.lines[0], "Tset") != NULL);
    TEST_ASSERT_TRUE(strstr(view.lines[0], "30.0") != NULL);
}

static void test_manual_mode_shows_manual_duty(void)
{
    display_view_input_t in = make_input();
    display_view_t       view;

    in.mode        = FAN_MODE_MANUAL;
    in.manual_duty_percent = 70U;

    TEST_ASSERT_EQUAL_INT(ERR_OK, display_view_build(&in, &view));
    TEST_ASSERT_TRUE(strstr(view.lines[0], "Duty") != NULL);
    TEST_ASSERT_TRUE(strstr(view.lines[0], "70") != NULL);
    check_all_lines_fit(&view);
}

/* -------------------------------------------------------------- 报警行 */

static void test_alarm_line_appears_only_when_needed(void)
{
    display_view_input_t in = make_input();
    display_view_t       view;

    TEST_ASSERT_EQUAL_INT(ERR_OK, display_view_build(&in, &view));
    TEST_ASSERT_EQUAL_UINT8(3U, view.line_count);   /* 无报警：不占行 */

    in.alarms = FAN_ALARM_TEMP_INVALID | FAN_ALARM_TACH_LOST;
    TEST_ASSERT_EQUAL_INT(ERR_OK, display_view_build(&in, &view));

    TEST_ASSERT_EQUAL_UINT8(4U, view.line_count);
    TEST_ASSERT_TRUE(strstr(view.lines[3], "!TEMP") != NULL);
    TEST_ASSERT_TRUE(strstr(view.lines[3], "!TACH") != NULL);
    check_all_lines_fit(&view);
}

/** 报警码全亮时最容易撑破行宽，单独钉一遍。 */
static void test_all_alarms_still_fit(void)
{
    display_view_input_t in = make_input();
    display_view_t       view;

    in.alarms = FAN_ALARM_TEMP_INVALID | FAN_ALARM_TACH_LOST
              | FAN_ALARM_RPM_NOT_TRACKING | FAN_ALARM_TEMP_UNCALIBRATED
              | FAN_ALARM_OLED_ABSENT;

    TEST_ASSERT_EQUAL_INT(ERR_OK, display_view_build(&in, &view));
    check_all_lines_fit(&view);
}

/* ---------------------------------------------------- 最坏情况的宽度 */

/**
 * 最宽的输入：五位数转速（FAN_CONTROL_RPM_MAX = 30000）、负温度、
 * 五位数目标转速、模式短码取最长的 "MANUAL"。
 */
static void test_widest_realistic_values_still_fit(void)
{
    display_view_input_t in = make_input();
    display_view_t       view;

    in.mode           = FAN_MODE_MANUAL;
    in.temp_milli_c   = -12300;    /* -12.3 */
    in.rpm            = 16456U;
    in.target_rpm     = 30000U;
    in.duty_percent   = 100U;
    in.manual_duty_percent = 100U;

    TEST_ASSERT_EQUAL_INT(ERR_OK, display_view_build(&in, &view));

    TEST_ASSERT_EQUAL_STRING("-12.3", view.temp_text);
    check_all_lines_fit(&view);

    /*
     * 只断言「不超宽」是不够的——截断本身也会让行变短，反而更容易通过。
     * 这里同时断言最宽的数值**确实完整出现**，把「被截断」也钉住。
     */
    TEST_ASSERT_NOT_NULL(strstr(view.lines[1], "16456"));
    TEST_ASSERT_NOT_NULL(strstr(view.lines[1], "100"));
    TEST_ASSERT_NOT_NULL(strstr(view.lines[2], "30000"));
    TEST_ASSERT_NOT_NULL(strstr(view.lines[0], "100"));

    /* 大字号那一行的数值也要放得下（最多 6 个字符含符号） */
    TEST_ASSERT_TRUE(strlen(view.temp_text) <= 6U);
}

static void test_invalid_rpm_shows_placeholder(void)
{
    display_view_input_t in = make_input();
    display_view_t       view;

    in.rpm_valid = false;

    TEST_ASSERT_EQUAL_INT(ERR_OK, display_view_build(&in, &view));
    TEST_ASSERT_TRUE(strstr(view.lines[1], "---") != NULL);
    check_all_lines_fit(&view);
}

static void test_rejects_null_arguments(void)
{
    const display_view_input_t in = make_input();
    display_view_t             view;

    TEST_ASSERT_EQUAL_INT(ERR_INVALID_PARAM, display_view_build(NULL, &view));
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_PARAM, display_view_build(&in, NULL));
}

/* ---------------------------------------------------------------- main */

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_builds_temperature_and_mode);
    RUN_TEST(test_invalid_temperature_shows_placeholder);
    RUN_TEST(test_auto_mode_shows_target_temperature);
    RUN_TEST(test_manual_mode_shows_manual_duty);

    RUN_TEST(test_alarm_line_appears_only_when_needed);
    RUN_TEST(test_all_alarms_still_fit);
    RUN_TEST(test_widest_realistic_values_still_fit);
    RUN_TEST(test_invalid_rpm_shows_placeholder);

    RUN_TEST(test_rejects_null_arguments);

    return UNITY_END();
}
