/**
 * @file display_view.c
 * @brief OLED 视图模型的实现。
 *
 * 约束与取舍见 display_view.h 顶部。这里只强调一点：
 *
 *   所有行都用 snprintf 写进**正好一行宽**的缓冲（DISPLAY_VIEW_LINE_CHARS）。
 *   超宽的后果是 snprintf 静默截断——看似是缺陷，其实是刻意选的：
 *   与其让字画到屏幕外（驱动不报错），不如在缓冲区边界上就截断，
 *   再由单元测试断言「任何输入下每行都 ≤21 字符」。截断在测试里会立刻暴露，
 *   而画到看不见的地方不会。
 */

#include "module/display_view/display_view.h"

#include <stdio.h>
#include <string.h>

/*
 * 本文件**故意**依赖 snprintf 的截断行为：每行的目标缓冲正好是一行的宽度
 * （DISPLAY_VIEW_LINE_CHARS），超宽的内容会被 snprintf 截到行内。
 *
 * 为什么这样是对的：ssd1306_write_string() 超宽时**不报错**，只是把多余的字
 * 画到看不见的地方（本工程踩过，README 有记录）。与其让字悄悄跑到屏幕外，
 * 不如在缓冲区边界上截断——而截断是**可测的**：
 * tests/test_display_view.c 用最宽的输入断言每一行的内容与宽度，
 * 一旦文案撑破行宽，CI 立刻变红。
 *
 * GCC 的 -Wformat-truncation 无法区分「有意的截断」与「写错了的格式」，
 * 所以在这里局部关掉它。作用范围仅限本文件，且不影响测试——测试会真的
 * 把超宽的输入喂进来验证结果。
 */
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif

/* ----------------------------------------------------------------- 内部 */

/** @brief 模式短码。与 telemetry 用同一套写法，便于串口/屏幕对照。 */
static const char *mode_name(fan_mode_t mode)
{
    switch (mode) {
    case FAN_MODE_INIT:     return "INIT";
    case FAN_MODE_OFF:      return "OFF";
    case FAN_MODE_MANUAL:   return "MANUAL";
    case FAN_MODE_AUTO:     return "AUTO";
    case FAN_MODE_DEGRADED: return "DEGR";
    case FAN_MODE_FAULT:    return "FAULT";
    default:                return "?";
    }
}

/**
 * @brief 把毫摄氏度拼成 "23.8" / "-3.1"。
 *
 * 与 telemetry 里那份是同一套算法，但**不共用**：display 要的是不带单位、
 * 尽量短的数值（要塞进大字号），telemetry 要的是带上下文的整行。
 * 共用会逼出「加一个 mode 参数」这种别扭设计，不如各自留一份。
 */
static void format_celsius(int32_t milli_c, char *buf, size_t cap)
{
    const char *sign  = "";
    int32_t     value = milli_c;

    if (value < 0) {
        sign  = "-";
        value = -value;
    }

    const int32_t tenths = (value + 50) / 100;

    if (tenths == 0) {
        sign = "";
    }

    (void)snprintf(buf, cap, "%s%ld.%ld", sign,
                   (long)(tenths / 10), (long)(tenths % 10));
}

/** @brief 把一个报警位映射成简短码。 */
static const char *alarm_code(fan_alarm_t bit)
{
    switch (bit) {
    case FAN_ALARM_TEMP_INVALID:      return "!TEMP";
    case FAN_ALARM_TACH_LOST:         return "!TACH";
    case FAN_ALARM_RPM_NOT_TRACKING:  return "!TRACK";
    case FAN_ALARM_TEMP_UNCALIBRATED: return "!CAL";
    case FAN_ALARM_OLED_ABSENT:       return "!OLED";
    default:                          return NULL;
    }
}

/** @brief 全部报警位，按「从严重到轻微」排列。 */
static const fan_alarm_t k_alarm_bits[] = {
    FAN_ALARM_TEMP_INVALID,
    FAN_ALARM_TACH_LOST,
    FAN_ALARM_RPM_NOT_TRACKING,
    FAN_ALARM_TEMP_UNCALIBRATED,
    FAN_ALARM_OLED_ABSENT,
};

/**
 * @brief 把所有置位的报警码拼成一行。
 *
 * @return bool 是否有内容可显示。
 */
static bool build_alarm_line(fan_alarm_t alarms, char *line, size_t cap)
{
    line[0] = '\0';

    size_t used = 0U;
    bool   any  = false;

    for (unsigned i = 0U; i < (sizeof(k_alarm_bits) / sizeof(k_alarm_bits[0])); i++) {
        if ((alarms & k_alarm_bits[i]) == 0U) {
            continue;
        }

        const char *code = alarm_code(k_alarm_bits[i]);
        if (code == NULL) {
            continue;
        }

        const size_t need = (used == 0U) ? 0U : 1U;   /* 分隔空格 */

        /* 放不下就停：宁可少显示一个码，也不越界。
           这里要预留结尾 NUL 的位置，所以是 `> cap` 而不是 `>= cap`。 */
        if ((used + need + strlen(code) + 1U) > cap) {
            break;
        }

        /*
         * 用 snprintf 的**实际返回长度**累加，不要按固定宽度估。
         * 报警码长 5~6 个字符，按 6 累加会在 5 字符的码后面留出一个空洞
         * （字符串的 NUL 落在中间），下一个码就被写到了字符串末尾之外——
         * 显示上表现为「只出现第一个报警码」，不报任何错。
         */
        const int n = snprintf(line + used, cap - used, "%s%s",
                               (need == 0U) ? "" : " ", code);

        if (n < 0) {
            break;
        }
        used += (size_t)n;
        any = true;
    }

    return any;
}

/* ----------------------------------------------------------------- 对外 */

error_t display_view_build(const display_view_input_t *in, display_view_t *out)
{
    if ((in == NULL) || (out == NULL)) {
        return ERR_INVALID_PARAM;
    }

    /*
     * 先整体清零。调用方（app_display）用 memcmp 比较「这一帧和上一帧是否
     * 完全一样」来决定要不要整屏刷新——而 snprintf 只写到自己那一段 NUL 为止，
     * 其后的字节保持栈上的残留值。不清零的话，这些残留字节会参与比较：
     * 内容其实没变，却被判成「变了」，白白多刷一次几十毫秒的屏。
     */
    *out = (display_view_t){0};

    /* ---- 大字号温度 ---- */
    if (in->temp_valid) {
        format_celsius(in->temp_milli_c, out->temp_text, sizeof(out->temp_text));
    } else {
        (void)snprintf(out->temp_text, sizeof(out->temp_text), "---");
    }

    /* ---- 模式短码 ---- */
    (void)snprintf(out->mode_text, sizeof(out->mode_text), "%s", mode_name(in->mode));

    /* ---- 第一行：用户能改的那个量 ---- */
    if (in->mode == FAN_MODE_MANUAL) {
        (void)snprintf(out->lines[0], DISPLAY_VIEW_LINE_CHARS,
                       "Duty  %u%%  (K1/K2)", (unsigned)in->manual_duty_percent);
    } else {
        char tset[16];

        format_celsius(in->target_temp_milli_c, tset, sizeof(tset));
        (void)snprintf(out->lines[0], DISPLAY_VIEW_LINE_CHARS,
                       "Tset  %s  (K1/K2)", tset);
    }

    /* ---- 第二行：闭环在不在工作，看这两个数 ---- */
    char rpm_text[8];

    if (in->rpm_valid) {
        (void)snprintf(rpm_text, sizeof(rpm_text), "%lu", (unsigned long)in->rpm);
    } else {
        (void)snprintf(rpm_text, sizeof(rpm_text), "---");
    }

    (void)snprintf(out->lines[1], DISPLAY_VIEW_LINE_CHARS,
                   "duty %3u%%   rpm %s", (unsigned)in->duty_percent, rpm_text);

    /* ---- 第三行：目标转速与误差（整定时看） ---- */
    char err_text[8];

    if (in->rpm_valid) {
        const long error = (long)in->target_rpm - (long)in->rpm;

        (void)snprintf(err_text, sizeof(err_text), "%ld", error);
    } else {
        (void)snprintf(err_text, sizeof(err_text), "---");
    }

    (void)snprintf(out->lines[2], DISPLAY_VIEW_LINE_CHARS,
                   "tgt %5lu  e %6s", (unsigned long)in->target_rpm, err_text);

    /* ---- 第四行：只在有报警时占位 ---- */
    out->line_count = 3U;

    if (build_alarm_line(in->alarms, out->lines[3], DISPLAY_VIEW_LINE_CHARS)) {
        out->line_count = 4U;
    } else {
        out->lines[3][0] = '\0';
    }

    return ERR_OK;
}
