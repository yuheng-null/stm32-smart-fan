/**
 * @file test_telemetry.c
 * @brief module/telemetry 的宿主机单元测试。
 *
 * 遥测帧是**机器**在读，所以字段顺序、分隔符、换行都必须逐字节钉死——
 * 上位机脚本靠这个解析，改一个字符就会让整条分析链悄悄对不上号。
 * 这里的断言用整串比较，而不是「包含某个子串」。
 */

#include "unity.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "interface/error.h"
#include "module/telemetry/telemetry.h"

void setUp(void) {}
void tearDown(void) {}

static telemetry_sample_t make_sample(void)
{
    telemetry_sample_t s = {0};

    s.seq          = 42U;
    s.tick_ms      = 8200U;
    s.mode         = FAN_MODE_AUTO;
    s.duty_percent = 48U;
    s.temp_milli_c = 31250;    /* 31.25 ℃ */
    s.temp_valid   = true;
    s.adc_raw      = 1980U;
    s.rpm          = 5980U;
    s.target_rpm   = 6000U;
    s.alarms       = FAN_ALARM_NONE;

    return s;
}

/* ---------------------------------------------------------------- CSV */

/**
 * 表头必须以 '#' 开头——绘图脚本靠这个把注释行跳过。
 * 同时核对字段名与 telemetry_format 的字段顺序一致。
 */
static void test_csv_header_is_a_comment(void)
{
    const char *header = telemetry_csv_header();

    TEST_ASSERT_NOT_NULL(header);
    TEST_ASSERT_EQUAL_CHAR('#', header[0]);
}

static void test_csv_frame_matches_wire_format_exactly(void)
{
    const telemetry_sample_t s = make_sample();
    char                     buf[128];
    size_t                   len = 0U;

    TEST_ASSERT_EQUAL_INT(ERR_OK, telemetry_format(&s, TELEMETRY_FMT_CSV,
                                                   buf, sizeof(buf), &len));

    TEST_ASSERT_EQUAL_STRING("@TEL,42,8200,3,48,31250,1980,5980,6000,0\r\n", buf);
    TEST_ASSERT_EQUAL_UINT32(strlen("@TEL,42,8200,3,48,31250,1980,5980,6000,0\r\n"), (uint32_t)len);
}

static void test_csv_encodes_alarm_bitmap(void)
{
    telemetry_sample_t s = make_sample();
    char               buf[128];
    size_t             len = 0U;

    s.alarms = FAN_ALARM_TEMP_INVALID | FAN_ALARM_TACH_LOST;   /* 1 | 2 = 3 */

    TEST_ASSERT_EQUAL_INT(ERR_OK, telemetry_format(&s, TELEMETRY_FMT_CSV,
                                                   buf, sizeof(buf), &len));
    TEST_ASSERT_TRUE(strstr(buf, ",3\r\n") != NULL);
}

static void test_csv_keeps_negative_temperature(void)
{
    telemetry_sample_t s = make_sample();
    char               buf[128];
    size_t             len = 0U;

    s.temp_milli_c = -3500;

    TEST_ASSERT_EQUAL_INT(ERR_OK, telemetry_format(&s, TELEMETRY_FMT_CSV,
                                                   buf, sizeof(buf), &len));
    TEST_ASSERT_TRUE(strstr(buf, ",-3500,") != NULL);
}

/* -------------------------------------------------------------- HUMAN */

static void test_human_line_contains_temperature_and_mode(void)
{
    const telemetry_sample_t s = make_sample();
    char                     buf[160];
    size_t                   len = 0U;

    TEST_ASSERT_EQUAL_INT(ERR_OK, telemetry_format(&s, TELEMETRY_FMT_HUMAN,
                                                   buf, sizeof(buf), &len));

    TEST_ASSERT_TRUE(strstr(buf, "31.2") != NULL || strstr(buf, "31.3") != NULL);
    TEST_ASSERT_TRUE(strstr(buf, "AUTO") != NULL);
    TEST_ASSERT_TRUE(strstr(buf, "\r\n") != NULL);
}

/** 温度无效时显示占位符，而不是一个看起来正常的数。 */
static void test_human_line_marks_invalid_temperature(void)
{
    telemetry_sample_t s = make_sample();
    char               buf[160];
    size_t             len = 0U;

    s.temp_valid = false;

    TEST_ASSERT_EQUAL_INT(ERR_OK, telemetry_format(&s, TELEMETRY_FMT_HUMAN,
                                                   buf, sizeof(buf), &len));
    TEST_ASSERT_TRUE(strstr(buf, "---") != NULL);
}

/* ------------------------------------------------------------ 边界 */

/**
 * 缓冲不够时必须**报错**，不能静默截断：被截断的帧会丢掉尾部的报警字段，
 * 而曲线看上去完全正常，问题会藏很久。
 */
static void test_small_buffer_reports_overflow(void)
{
    const telemetry_sample_t s = make_sample();
    char                     buf[8];
    size_t                   len = 0U;

    TEST_ASSERT_EQUAL_INT(ERR_OVERFLOW, telemetry_format(&s, TELEMETRY_FMT_CSV,
                                                         buf, sizeof(buf), &len));
}

static void test_rejects_null_arguments(void)
{
    const telemetry_sample_t s = make_sample();
    char                     buf[64];
    size_t                   len = 0U;

    TEST_ASSERT_EQUAL_INT(ERR_INVALID_PARAM,
                          telemetry_format(NULL, TELEMETRY_FMT_CSV, buf, sizeof(buf), &len));
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_PARAM,
                          telemetry_format(&s, TELEMETRY_FMT_CSV, NULL, sizeof(buf), &len));
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_PARAM,
                          telemetry_format(&s, TELEMETRY_FMT_CSV, buf, sizeof(buf), NULL));
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_PARAM,
                          telemetry_format(&s, TELEMETRY_FMT_CSV, buf, 0U, &len));
}

/* ---------------------------------------------------------------- main */

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_csv_header_is_a_comment);
    RUN_TEST(test_csv_frame_matches_wire_format_exactly);
    RUN_TEST(test_csv_encodes_alarm_bitmap);
    RUN_TEST(test_csv_keeps_negative_temperature);

    RUN_TEST(test_human_line_contains_temperature_and_mode);
    RUN_TEST(test_human_line_marks_invalid_temperature);

    RUN_TEST(test_small_buffer_reports_overflow);
    RUN_TEST(test_rejects_null_arguments);

    return UNITY_END();
}
