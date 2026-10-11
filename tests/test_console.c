/**
 * @file test_console.c
 * @brief module/console 的宿主机单元测试。
 *
 * 命令解析器是最容易被「顺手写残」的一层：一个越界的 memcpy、一次把 `\r\n`
 * 当成两条命令、一个漏掉的边界判断，在板子上表现为「偶尔重启」或「命令执行两次」，
 * 极难定位。这里把每一种切分方式都喂一遍。
 */

#include "unity.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "interface/error.h"
#include "module/console/console.h"

void setUp(void) {}
void tearDown(void) {}

/** 一次喂入产生的所有完整命令。 */
typedef struct {
    console_command_t cmds[8];
    error_t           results[8];
    uint8_t           count;
} parsed_t;

static void parse_string(console_parser_t *parser, const char *text, parsed_t *out)
{
    out->count = 0U;

    for (const char *c = text; *c != '\0'; c++) {
        console_command_t cmd;

        const error_t result = console_parser_feed(parser, (uint8_t)*c, &cmd);

        if (result == ERR_NOT_READY) {
            continue;
        }

        if (out->count < (uint8_t)(sizeof(out->cmds) / sizeof(out->cmds[0]))) {
            out->results[out->count] = result;
            out->cmds[out->count]    = cmd;
            out->count++;
        }
    }
}

/* ------------------------------------------------------------------ IR */

/** `IR` 不带参数 = 看状态；带 LEARN 0|1 = 开关学习模式。 */
static void test_ir_status_and_learn(void)
{
    console_parser_t parser = {0};
    parsed_t         got;

    parse_string(&parser, "IR\r\n", &got);
    TEST_ASSERT_EQUAL_UINT8(1U, got.count);
    TEST_ASSERT_EQUAL_INT(ERR_OK, got.results[0]);
    TEST_ASSERT_EQUAL_INT(CONSOLE_CMD_IR, got.cmds[0].id);
    TEST_ASSERT_EQUAL_INT(CONSOLE_IR_STATUS, got.cmds[0].arg_i[0]);

    parse_string(&parser, "IR LEARN 1\r\n", &got);
    TEST_ASSERT_EQUAL_UINT8(1U, got.count);
    TEST_ASSERT_EQUAL_INT(ERR_OK, got.results[0]);
    TEST_ASSERT_EQUAL_INT(CONSOLE_CMD_IR, got.cmds[0].id);
    TEST_ASSERT_EQUAL_INT(CONSOLE_IR_LEARN_ON, got.cmds[0].arg_i[0]);

    parse_string(&parser, "IR LEARN 0\r\n", &got);
    TEST_ASSERT_EQUAL_UINT8(1U, got.count);
    TEST_ASSERT_EQUAL_INT(CONSOLE_CMD_IR, got.cmds[0].id);
    TEST_ASSERT_EQUAL_INT(CONSOLE_IR_LEARN_OFF, got.cmds[0].arg_i[0]);

    /* 大小写不敏感，与前缀一致 */
    parse_string(&parser, "ir learn 1\n", &got);
    TEST_ASSERT_EQUAL_UINT8(1U, got.count);
    TEST_ASSERT_EQUAL_INT(CONSOLE_IR_LEARN_ON, got.cmds[0].arg_i[0]);
}

static void test_ir_rejects_bad_arguments(void)
{
    console_parser_t parser = {0};
    parsed_t         got;

    /* 学习模式只有 0/1 两档，别的既不是「非法参数」也不是「静默接受」 */
    parse_string(&parser, "IR LEARN 2\r\n", &got);
    TEST_ASSERT_EQUAL_UINT8(1U, got.count);
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_PARAM, got.results[0]);
    TEST_ASSERT_EQUAL_INT(CONSOLE_CMD_UNKNOWN, got.cmds[0].id);

    parse_string(&parser, "IR LEARN\r\n", &got);
    TEST_ASSERT_EQUAL_UINT8(1U, got.count);
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_PARAM, got.results[0]);

    parse_string(&parser, "IR BOGUS\r\n", &got);
    TEST_ASSERT_EQUAL_UINT8(1U, got.count);
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_PARAM, got.results[0]);
    TEST_ASSERT_EQUAL_INT(CONSOLE_CMD_UNKNOWN, got.cmds[0].id);
}

/* ------------------------------------------------------------ 基本命令 */

static void test_help_and_status(void)
{
    console_parser_t parser = {0};
    parsed_t         got;

    parse_string(&parser, "?\r\n", &got);
    TEST_ASSERT_EQUAL_UINT8(1U, got.count);
    TEST_ASSERT_EQUAL_INT(ERR_OK, got.results[0]);
    TEST_ASSERT_EQUAL_INT(CONSOLE_CMD_HELP, got.cmds[0].id);

    parse_string(&parser, "STATUS\n", &got);
    TEST_ASSERT_EQUAL_UINT8(1U, got.count);
    TEST_ASSERT_EQUAL_INT(CONSOLE_CMD_STATUS, got.cmds[0].id);
}

static void test_reset(void)
{
    console_parser_t parser = {0};
    parsed_t         got;

    parse_string(&parser, "RESET\r\n", &got);
    TEST_ASSERT_EQUAL_UINT8(1U, got.count);
    TEST_ASSERT_EQUAL_INT(CONSOLE_CMD_RESET, got.cmds[0].id);
}

/* ---------------------------------------------------------------- MODE */

static void test_mode_arguments_and_case(void)
{
    console_parser_t parser = {0};
    parsed_t         got;

    parse_string(&parser, "MODE AUTO\r\n", &got);
    TEST_ASSERT_EQUAL_UINT8(1U, got.count);
    TEST_ASSERT_EQUAL_INT(CONSOLE_CMD_MODE, got.cmds[0].id);
    TEST_ASSERT_EQUAL_INT32(1, got.cmds[0].arg_i[0]);
    TEST_ASSERT_EQUAL_STRING("MODE", got.cmds[0].verb);

    /* 大小写不敏感 */
    parse_string(&parser, "mode manual\n", &got);
    TEST_ASSERT_EQUAL_INT(CONSOLE_CMD_MODE, got.cmds[0].id);
    TEST_ASSERT_EQUAL_INT32(2, got.cmds[0].arg_i[0]);

    parse_string(&parser, "MODE off\n", &got);
    TEST_ASSERT_EQUAL_INT32(0, got.cmds[0].arg_i[0]);
}

static void test_mode_rejects_bad_or_missing_argument(void)
{
    console_parser_t parser = {0};
    parsed_t         got;

    parse_string(&parser, "MODE\n", &got);
    TEST_ASSERT_EQUAL_UINT8(1U, got.count);
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_PARAM, got.results[0]);
    TEST_ASSERT_EQUAL_INT(CONSOLE_CMD_UNKNOWN, got.cmds[0].id);

    parse_string(&parser, "MODE TURBO\n", &got);
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_PARAM, got.results[0]);
}

/* -------------------------------------------------------------- MANUAL */

static void test_manual_parses_integer(void)
{
    console_parser_t parser = {0};
    parsed_t         got;

    parse_string(&parser, "MANUAL 60\r\n", &got);
    TEST_ASSERT_EQUAL_UINT8(1U, got.count);
    TEST_ASSERT_EQUAL_INT(CONSOLE_CMD_MANUAL, got.cmds[0].id);
    TEST_ASSERT_EQUAL_INT32(60, got.cmds[0].arg_i[0]);

    parse_string(&parser, "MANUAL xyz\n", &got);
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_PARAM, got.results[0]);
}

/* ---------------------------------------------------------------- TEMP */

static void test_temp_parses_degrees_into_millidegrees(void)
{
    console_parser_t parser = {0};
    parsed_t         got;

    parse_string(&parser, "TEMP 32.5\r\n", &got);
    TEST_ASSERT_EQUAL_INT(CONSOLE_CMD_TEMP, got.cmds[0].id);
    TEST_ASSERT_EQUAL_INT32(32500, got.cmds[0].arg_i[0]);

    /* 没有小数点时按整度解释，而不是「毫摄氏度」 */
    parse_string(&parser, "TEMP 32\n", &got);
    TEST_ASSERT_EQUAL_INT32(32000, got.cmds[0].arg_i[0]);

    parse_string(&parser, "TEMP -3.5\n", &got);
    TEST_ASSERT_EQUAL_INT32(-3500, got.cmds[0].arg_i[0]);

    parse_string(&parser, "TEMP 32.55\n", &got);
    TEST_ASSERT_EQUAL_INT32(32550, got.cmds[0].arg_i[0]);

    parse_string(&parser, "TEMP\n", &got);
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_PARAM, got.results[0]);
}

/* ----------------------------------------------------------------- PID */

static void test_pid_parses_two_floats(void)
{
    console_parser_t parser = {0};
    parsed_t         got;

    parse_string(&parser, "PID 0.0037 0.0123\r\n", &got);
    TEST_ASSERT_EQUAL_UINT8(1U, got.count);
    TEST_ASSERT_EQUAL_INT(CONSOLE_CMD_PID, got.cmds[0].id);
    TEST_ASSERT_EQUAL_UINT8(2U, got.cmds[0].arg_count);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.0037f, got.cmds[0].arg_f[0]);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.0123f, got.cmds[0].arg_f[1]);

    parse_string(&parser, "PID 1\n", &got);
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_PARAM, got.results[0]);
}

/* -------------------------------------------------------------- STREAM */

static void test_stream_accepts_only_zero_or_one(void)
{
    console_parser_t parser = {0};
    parsed_t         got;

    parse_string(&parser, "STREAM 1\r\n", &got);
    TEST_ASSERT_EQUAL_INT(CONSOLE_CMD_STREAM, got.cmds[0].id);
    TEST_ASSERT_EQUAL_INT32(1, got.cmds[0].arg_i[0]);

    parse_string(&parser, "STREAM 2\n", &got);
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_PARAM, got.results[0]);
}

/* ------------------------------------------------------------ 边界情况 */

static void test_unknown_verb_is_reported_but_not_an_error(void)
{
    console_parser_t parser = {0};
    parsed_t         got;

    parse_string(&parser, "FLY\r\n", &got);

    TEST_ASSERT_EQUAL_UINT8(1U, got.count);
    /* 认不出命令词不算「参数非法」：app 只需要回一句「不认识」 */
    TEST_ASSERT_EQUAL_INT(ERR_OK, got.results[0]);
    TEST_ASSERT_EQUAL_INT(CONSOLE_CMD_UNKNOWN, got.cmds[0].id);
    TEST_ASSERT_EQUAL_STRING("FLY", got.cmds[0].verb);
}

/** CRLF 的行结束会让「行结束」触发两次，空行必须不产生命令。 */
static void test_crlf_produces_exactly_one_command(void)
{
    console_parser_t parser = {0};
    parsed_t         got;

    parse_string(&parser, "STATUS\r\n", &got);
    TEST_ASSERT_EQUAL_UINT8(1U, got.count);

    parse_string(&parser, "\r\n\n\r\n", &got);
    TEST_ASSERT_EQUAL_UINT8(0U, got.count);
}

static void test_two_commands_in_one_buffer(void)
{
    console_parser_t parser = {0};
    parsed_t         got;

    parse_string(&parser, "MODE AUTO\r\nSTATUS\r\n", &got);

    TEST_ASSERT_EQUAL_UINT8(2U, got.count);
    TEST_ASSERT_EQUAL_INT(CONSOLE_CMD_MODE, got.cmds[0].id);
    TEST_ASSERT_EQUAL_INT(CONSOLE_CMD_STATUS, got.cmds[1].id);
}

/** 超长行必须被整行丢弃，且绝不能越界写缓冲。 */
static void test_overlong_line_is_rejected_without_overrun(void)
{
    console_parser_t parser = {0};
    parsed_t         got;

    /* 200 个 'A' 远超 CONSOLE_LINE_MAX，随后接一条正常命令 */
    char oversized[220];
    memset(oversized, 'A', 200U);
    oversized[200] = '\0';

    parse_string(&parser, oversized, &got);
    TEST_ASSERT_EQUAL_UINT8(0U, got.count);       /* 还没遇到行结束 */

    parse_string(&parser, "\r\n", &got);
    TEST_ASSERT_EQUAL_UINT8(1U, got.count);
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_PARAM, got.results[0]);
    TEST_ASSERT_EQUAL_INT(CONSOLE_CMD_UNKNOWN, got.cmds[0].id);

    /* 溢出之后解析器要能继续正常工作 */
    parse_string(&parser, "STATUS\r\n", &got);
    TEST_ASSERT_EQUAL_UINT8(1U, got.count);
    TEST_ASSERT_EQUAL_INT(CONSOLE_CMD_STATUS, got.cmds[0].id);
}

static void test_extra_whitespace_is_tolerated(void)
{
    console_parser_t parser = {0};
    parsed_t         got;

    parse_string(&parser, "   MODE    AUTO   \r\n", &got);
    TEST_ASSERT_EQUAL_UINT8(1U, got.count);
    TEST_ASSERT_EQUAL_INT(CONSOLE_CMD_MODE, got.cmds[0].id);
    TEST_ASSERT_EQUAL_INT32(1, got.cmds[0].arg_i[0]);
}

static void test_reset_clears_partial_line(void)
{
    console_parser_t parser = {0};
    parsed_t         got;

    parse_string(&parser, "MODE AU", &got);
    TEST_ASSERT_EQUAL_UINT8(0U, got.count);

    console_parser_reset(&parser);
    parse_string(&parser, "STATUS\r\n", &got);
    TEST_ASSERT_EQUAL_UINT8(1U, got.count);
    TEST_ASSERT_EQUAL_INT(CONSOLE_CMD_STATUS, got.cmds[0].id);
}

static void test_help_text_is_not_empty(void)
{
    const char *help = console_help_text();

    TEST_ASSERT_NOT_NULL(help);
    TEST_ASSERT_TRUE(strlen(help) > 0U);
}

/* ---------------------------------------------------------------- main */

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_help_and_status);
    RUN_TEST(test_reset);
    RUN_TEST(test_mode_arguments_and_case);
    RUN_TEST(test_mode_rejects_bad_or_missing_argument);
    RUN_TEST(test_manual_parses_integer);
    RUN_TEST(test_temp_parses_degrees_into_millidegrees);
    RUN_TEST(test_pid_parses_two_floats);
    RUN_TEST(test_stream_accepts_only_zero_or_one);

    RUN_TEST(test_unknown_verb_is_reported_but_not_an_error);
    RUN_TEST(test_crlf_produces_exactly_one_command);
    RUN_TEST(test_two_commands_in_one_buffer);
    RUN_TEST(test_overlong_line_is_rejected_without_overrun);
    RUN_TEST(test_extra_whitespace_is_tolerated);
    RUN_TEST(test_reset_clears_partial_line);
    RUN_TEST(test_ir_status_and_learn);
    RUN_TEST(test_ir_rejects_bad_arguments);

    RUN_TEST(test_help_text_is_not_empty);

    return UNITY_END();
}
