/**
 * @file console.c
 * @brief 串口行命令解析器的实现。
 *
 * 协议与设计理由见 console.h 顶部。实现上有两处值得说明：
 *
 * ── 1. 数字解析全部自己写，不依赖 newlib ─────────────────────────────
 *   温度走**整数**十进制（"32.5" → 32500 毫摄氏度），全程整数运算、逐位精确；
 *   PID 增益才用浮点（它本来就是小数增益）。这样既绕开 newlib-nano 的浮点
 *   格式化开关，也避免 "32.5" 在某些 libc 上被解析成 32.499999 这类幺蛾子。
 *
 * ── 2. 空行不产生命令 ────────────────────────────────────────────────
 *   `\r` 与 `\n` 都算行结束，于是 `\r\n` 会触发两次「行结束」。若不特判空行，
 *   一条命令会被执行两次——切模式两次、继电器翻转两次，很难排查。
 * ────────────────────────────────────────────────────────────────────
 */

#include "module/console/console.h"

#include <stddef.h>
#include <string.h>

/** MODE 命令的参数编码（放在 arg_i[0]）。 */
#define CONSOLE_MODE_OFF    (0)
#define CONSOLE_MODE_AUTO   (1)
#define CONSOLE_MODE_MANUAL (2)


/* ------------------------------------------------------------ 词法工具 */

static bool is_digit(char c)
{
    return (c >= '0') && (c <= '9');
}

/** @brief 跳过空白。 */
static const char *skip_space(const char *s)
{
    while ((*s == ' ') || (*s == '\t')) {
        s++;
    }
    return s;
}

/**
 * @brief 从 cursor 处取下一个 token。
 *
 * @param cursor 输入输出：进入时指向待解析处，返回时指向 token 之后。
 * @param len    输出 token 长度（0 表示已无 token）。
 * @return const char* token 起点（**不保证以 NUL 结尾**，长度由 len 给出）。
 */
static const char *token_next(const char **cursor, uint8_t *len)
{
    const char *start = skip_space(*cursor);
    uint8_t     n     = 0U;

    while ((start[n] != '\0') && (start[n] != ' ') && (start[n] != '\t')) {
        n++;
    }

    *cursor = start + n;
    *len    = n;

    return start;
}

/** @brief 大小写不敏感地比较一个 token 与字面量。 */
static bool token_equals(const char *tok, uint8_t len, const char *literal)
{
    if (strlen(literal) != (size_t)len) {
        return false;
    }

    for (uint8_t i = 0U; i < len; i++) {
        char c = tok[i];

        if ((c >= 'a') && (c <= 'z')) {
            c = (char)((c - 'a') + 'A');
        }
        if (c != literal[i]) {
            return false;
        }
    }

    return true;
}

/* ------------------------------------------------------------ 数字解析 */

/** @brief 解析一个有符号十进制整数。 */
static bool parse_int32(const char *s, uint8_t len, int32_t *out)
{
    if (len == 0U) {
        return false;
    }

    uint8_t i   = 0U;
    bool    neg = false;

    if ((s[0] == '+') || (s[0] == '-')) {
        neg = (s[0] == '-');
        i   = 1U;
    }
    if (i >= len) {
        return false;
    }

    int64_t value = 0;

    for (; i < len; i++) {
        if (!is_digit(s[i])) {
            return false;
        }
        value = (value * 10) + (s[i] - '0');
        if (value > 2147483647LL) {
            return false;
        }
    }

    *out = (int32_t)(neg ? -value : value);

    return true;
}

/**
 * @brief 解析「摄氏度」文本并转成毫摄氏度。
 *
 * "32" → 32000、"32.5" → 32500、"-3.5" → -3500。
 * 超过三位的小数会被忽略（毫摄氏度分辨率的极限）。
 */
static bool parse_milli_degrees(const char *s, uint8_t len, int32_t *out)
{
    if (len == 0U) {
        return false;
    }

    uint8_t i   = 0U;
    bool    neg = false;

    if ((s[0] == '+') || (s[0] == '-')) {
        neg = (s[0] == '-');
        i   = 1U;
    }
    if (i >= len) {
        return false;
    }

    int64_t whole = 0;
    bool    any   = false;

    for (; (i < len) && (s[i] != '.'); i++) {
        if (!is_digit(s[i])) {
            return false;
        }
        whole = (whole * 10) + (s[i] - '0');
        any   = true;
        if (whole > 100000) {
            return false;               /* 明显越界，早退，防溢出 */
        }
    }

    int64_t frac  = 0;
    int32_t scale = 100;                 /* 第一位小数 = 100 毫摄氏度 */

    if ((i < len) && (s[i] == '.')) {
        i++;
        for (; i < len; i++) {
            if (!is_digit(s[i])) {
                return false;
            }
            if (scale > 0) {
                frac += (int64_t)(s[i] - '0') * scale;
                scale /= 10;
            }
            any = true;
        }
    }

    if (!any) {
        return false;
    }

    const int64_t milli = (whole * 1000) + frac;

    if (milli > ((int64_t)INT32_MAX / 2)) {
        return false;
    }

    *out = (int32_t)(neg ? -milli : milli);

    return true;
}

/** @brief 解析一个十进制浮点数（供 PID 增益使用）。 */
static bool parse_float(const char *s, uint8_t len, float *out)
{
    if (len == 0U) {
        return false;
    }

    uint8_t i   = 0U;
    bool    neg = false;

    if ((s[0] == '+') || (s[0] == '-')) {
        neg = (s[0] == '-');
        i   = 1U;
    }
    if (i >= len) {
        return false;
    }

    float value = 0.0f;
    bool  any   = false;

    for (; (i < len) && (s[i] != '.'); i++) {
        if (!is_digit(s[i])) {
            return false;
        }
        value = (value * 10.0f) + (float)(s[i] - '0');
        any   = true;
    }

    if ((i < len) && (s[i] == '.')) {
        i++;
        float scale = 0.1f;

        for (; i < len; i++) {
            if (!is_digit(s[i])) {
                return false;
            }
            value += (float)(s[i] - '0') * scale;
            scale *= 0.1f;
            any = true;
        }
    }

    if (!any) {
        return false;
    }

    *out = neg ? -value : value;

    return true;
}

/* ------------------------------------------------------------ 行解析 */

/** @brief 把一个 token 转成大写并抄进 out->verb。 */
static void copy_verb(const char *token, uint8_t len, console_command_t *out)
{
    uint8_t n = (len < (uint8_t)(CONSOLE_VERB_MAX - 1U)) ? len
                                                         : (uint8_t)(CONSOLE_VERB_MAX - 1U);

    for (uint8_t i = 0U; i < n; i++) {
        char c = token[i];

        if ((c >= 'a') && (c <= 'z')) {
            c = (char)((c - 'a') + 'A');
        }
        out->verb[i] = c;
    }
    out->verb[n] = '\0';
}

/** @brief 解析一整行（在收到行结束符时调用）。 */
static error_t console_finish_line(console_parser_t *parser, console_command_t *out)
{
    out->id        = CONSOLE_CMD_NONE;
    out->arg_count = 0U;
    out->arg_i[0]  = 0;
    out->arg_i[1]  = 0;
    out->arg_f[0]  = 0.0f;
    out->arg_f[1]  = 0.0f;
    out->verb[0]   = '\0';

    if (parser->overflow) {
        out->id = CONSOLE_CMD_UNKNOWN;
        return ERR_INVALID_PARAM;
    }

    parser->line[parser->len] = '\0';

    const char *cursor = parser->line;
    uint8_t     verb_len = 0U;
    const char *verb     = token_next(&cursor, &verb_len);

    if (verb_len == 0U) {
        return ERR_NOT_READY;       /* 空行（\r\n 的后半截）不产生命令 */
    }

    copy_verb(verb, verb_len, out);

    /* ---- 无参命令 ---- */
    if (strcmp(out->verb, "?") == 0) {
        out->id = CONSOLE_CMD_HELP;
        return ERR_OK;
    }
    if (strcmp(out->verb, "STATUS") == 0) {
        out->id = CONSOLE_CMD_STATUS;
        return ERR_OK;
    }
    if (strcmp(out->verb, "RESET") == 0) {
        out->id = CONSOLE_CMD_RESET;
        return ERR_OK;
    }

    /* ---- IR [LEARN <0|1>] ---- */
    if (strcmp(out->verb, "IR") == 0) {
        uint8_t     sub_len = 0U;
        const char *sub     = token_next(&cursor, &sub_len);

        /* 不带参数 = 看状态。 */
        if (sub_len == 0U) {
            out->arg_i[0] = CONSOLE_IR_STATUS;
            out->arg_count = 0U;
            out->id        = CONSOLE_CMD_IR;
            return ERR_OK;
        }

        if (token_equals(sub, sub_len, "LEARN")) {
            uint8_t     arg_len = 0U;
            const char *arg     = token_next(&cursor, &arg_len);
            int32_t     on      = 0;

            if (!parse_int32(arg, arg_len, &on) || ((on != 0) && (on != 1))) {
                out->id = CONSOLE_CMD_UNKNOWN;
                return ERR_INVALID_PARAM;
            }

            out->arg_i[0]  = (on == 1) ? CONSOLE_IR_LEARN_ON : CONSOLE_IR_LEARN_OFF;
            out->arg_count = 1U;
            out->id        = CONSOLE_CMD_IR;
            return ERR_OK;
        }

        out->id = CONSOLE_CMD_UNKNOWN;
        return ERR_INVALID_PARAM;
    }

    /* ---- MODE OFF|AUTO|MANUAL ---- */
    if (strcmp(out->verb, "MODE") == 0) {
        uint8_t     arg_len = 0U;
        const char *arg     = token_next(&cursor, &arg_len);

        if (token_equals(arg, arg_len, "OFF")) {
            out->arg_i[0] = CONSOLE_MODE_OFF;
        } else if (token_equals(arg, arg_len, "AUTO")) {
            out->arg_i[0] = CONSOLE_MODE_AUTO;
        } else if (token_equals(arg, arg_len, "MANUAL")) {
            out->arg_i[0] = CONSOLE_MODE_MANUAL;
        } else {
            out->id = CONSOLE_CMD_UNKNOWN;
            return ERR_INVALID_PARAM;
        }

        out->arg_count = 1U;
        out->id        = CONSOLE_CMD_MODE;
        return ERR_OK;
    }

    /* ---- MANUAL <0..100> ---- */
    if (strcmp(out->verb, "MANUAL") == 0) {
        uint8_t     arg_len = 0U;
        const char *arg     = token_next(&cursor, &arg_len);

        if (!parse_int32(arg, arg_len, &out->arg_i[0])) {
            out->id = CONSOLE_CMD_UNKNOWN;
            return ERR_INVALID_PARAM;
        }

        out->arg_count = 1U;
        out->id        = CONSOLE_CMD_MANUAL;
        return ERR_OK;
    }

    /* ---- TEMP <摄氏度> ---- */
    if (strcmp(out->verb, "TEMP") == 0) {
        uint8_t     arg_len = 0U;
        const char *arg     = token_next(&cursor, &arg_len);

        if (!parse_milli_degrees(arg, arg_len, &out->arg_i[0])) {
            out->id = CONSOLE_CMD_UNKNOWN;
            return ERR_INVALID_PARAM;
        }

        out->arg_count = 1U;
        out->id        = CONSOLE_CMD_TEMP;
        return ERR_OK;
    }

    /* ---- PID <kp> <ki> ---- */
    if (strcmp(out->verb, "PID") == 0) {
        uint8_t     len_a = 0U;
        uint8_t     len_b = 0U;
        const char *arg_a = token_next(&cursor, &len_a);
        const char *arg_b = token_next(&cursor, &len_b);

        if (!parse_float(arg_a, len_a, &out->arg_f[0])
            || !parse_float(arg_b, len_b, &out->arg_f[1])) {
            out->id = CONSOLE_CMD_UNKNOWN;
            return ERR_INVALID_PARAM;
        }

        out->arg_count = 2U;
        out->id        = CONSOLE_CMD_PID;
        return ERR_OK;
    }

    /* ---- STREAM <0|1> ---- */
    if (strcmp(out->verb, "STREAM") == 0) {
        uint8_t     arg_len = 0U;
        const char *arg     = token_next(&cursor, &arg_len);

        if (!parse_int32(arg, arg_len, &out->arg_i[0])
            || ((out->arg_i[0] != 0) && (out->arg_i[0] != 1))) {
            out->id = CONSOLE_CMD_UNKNOWN;
            return ERR_INVALID_PARAM;
        }

        out->arg_count = 1U;
        out->id        = CONSOLE_CMD_STREAM;
        return ERR_OK;
    }

    /* 认不出的命令词：不算「参数非法」，交给 app 回一句「不认识」即可。 */
    out->id = CONSOLE_CMD_UNKNOWN;
    return ERR_OK;
}

/* ----------------------------------------------------------------- 对外 */

void console_parser_reset(console_parser_t *parser)
{
    if (parser == NULL) {
        return;
    }

    parser->len      = 0U;
    parser->overflow = false;
    parser->line[0]  = '\0';
}

error_t console_parser_feed(console_parser_t *parser, uint8_t ch, console_command_t *out)
{
    if ((parser == NULL) || (out == NULL)) {
        return ERR_INVALID_PARAM;
    }

    if ((ch == (uint8_t)'\r') || (ch == (uint8_t)'\n')) {
        const error_t result = console_finish_line(parser, out);

        parser->len      = 0U;
        parser->overflow = false;

        return result;
    }

    if (parser->len < (uint8_t)(CONSOLE_LINE_MAX - 1U)) {
        parser->line[parser->len] = (char)ch;
        parser->len++;
    } else {
        /* 超出缓冲：丢弃到行尾，并记下「这行不可信」。
           不截断后照常解析——半条命令比没有命令更危险。 */
        parser->overflow = true;
    }

    return ERR_NOT_READY;
}

const char *console_help_text(void)
{
    static const char help[] =
        "\r\n命令（行协议，大小写不敏感）:\r\n"
        "  ?              显示本表\r\n"
        "  STATUS         打印一行完整状态\r\n"
        "  MODE OFF|AUTO|MANUAL\r\n"
        "  MANUAL <0..100>   手动占空比（%）；1~19 会被抬到 20\r\n"
        "  TEMP <摄氏度>     设定目标温度，如 32.5\r\n"
        "  PID <kp> <ki>     在线整定内环\r\n"
        "  STREAM <0|1>      0=人类可读行，1=CSV 遥测帧\r\n"
        "  RESET          清报警并回到 AUTO\r\n"
        "  IR             遥控接收状态（帧数/拒帧原因/丢沿）\r\n"
        "  IR LEARN <0|1> 1=学习模式：只回显收到的遥控码，不执行动作\r\n"
        "按键：K1 短按调高目标/加档，K2 短按调低/减档；\r\n"
        "      K1 长按循环 AUTO→MANUAL→OFF；K2 长按静音（FAULT 下为复位）。\r\n"
        "\r\n";

    return help;
}
