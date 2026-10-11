/**
 * @file telemetry.c
 * @brief 遥测格式化的实现。
 *
 * 格式与设计理由见 telemetry.h 顶部。这里只说明温度为什么要单独拼：
 *
 *   `%f` 在 newlib-nano 下默认不可用，而温度是唯一带小数的字段。
 *   把「毫摄氏度整数」拆成 `整数部分.小数部分` 拼出来，既避免浮点格式化，
 *   又保证负温度、四舍五入的行为在 PC 与板子上完全一致。
 */

#include "module/telemetry/telemetry.h"

#include <stdio.h>
#include <string.h>

/* ----------------------------------------------------------------- 内部 */

/** @brief 模式短码（遥测与显示共用同一套写法，便于对照排查）。 */
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
 * 四舍五入到 0.1 ℃；并规避 "-0.0" 这种看着像 bug 的输出。
 */
static void format_celsius(int32_t milli_c, char *buf, size_t cap)
{
    const char *sign  = "";
    int32_t     value = milli_c;

    if (value < 0) {
        sign  = "-";
        value = -value;
    }

    const int32_t tenths = (value + 50) / 100;   /* 先加半步再整除 = 四舍五入 */

    if (tenths == 0) {
        sign = "";
    }

    (void)snprintf(buf, cap, "%s%ld.%ld", sign,
                   (long)(tenths / 10), (long)(tenths % 10));
}

/* ----------------------------------------------------------------- 对外 */

error_t telemetry_format(const telemetry_sample_t *sample, telemetry_format_t format,
                         char *buf, size_t cap, size_t *out_len)
{
    if ((sample == NULL) || (buf == NULL) || (out_len == NULL)) {
        return ERR_INVALID_PARAM;
    }
    if (cap == 0U) {
        return ERR_INVALID_PARAM;
    }

    int written;

    if (format == TELEMETRY_FMT_CSV) {
        /*
         * 字段顺序即契约。表头与上位机脚本都按这个顺序解析，改这里必须同步改表头。
         * 温度即使无效也照发原值（配合后端的有效标志），因为「当时读数是多少」
         * 对复盘故障有用——把无效帧整个丢掉反而丢掉了线索。
         */
        written = snprintf(buf, cap, "@TEL,%u,%lu,%d,%u,%ld,%u,%lu,%lu,%lu\r\n",
                           (unsigned)sample->seq,
                           (unsigned long)sample->tick_ms,
                           (int)sample->mode,
                           (unsigned)sample->duty_percent,
                           (long)sample->temp_milli_c,
                           (unsigned)sample->adc_raw,
                           (unsigned long)sample->rpm,
                           (unsigned long)sample->target_rpm,
                           (unsigned long)sample->alarms);
    } else {
        char temp_text[16];

        if (sample->temp_valid) {
            format_celsius(sample->temp_milli_c, temp_text, sizeof(temp_text));
        } else {
            (void)snprintf(temp_text, sizeof(temp_text), "---");
        }

        written = snprintf(buf, cap,
                           "[%6lu ms] %-6s T %sC  adc %4u  duty %3u%%  rpm %lu->%lu  alarms 0x%02lX\r\n",
                           (unsigned long)sample->tick_ms,
                           mode_name(sample->mode),
                           temp_text,
                           (unsigned)sample->adc_raw,
                           (unsigned)sample->duty_percent,
                           (unsigned long)sample->rpm,
                           (unsigned long)sample->target_rpm,
                           (unsigned long)sample->alarms);
    }

    if (written < 0) {
        return ERR_INVALID_PARAM;       /* 编码错误 */
    }

    if ((size_t)written >= cap) {
        /* snprintf 已按 cap 截断，这里如实报错，让调用方有机会放大缓冲。 */
        *out_len = cap - 1U;
        return ERR_OVERFLOW;
    }

    *out_len = (size_t)written;

    return ERR_OK;
}

const char *telemetry_csv_header(void)
{
    return "# @TEL,seq,t_ms,mode,duty,temp_mc,adc,rpm,target_rpm,alarms\r\n";
}
