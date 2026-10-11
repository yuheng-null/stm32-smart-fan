/**
 * @file telemetry.h
 * @brief 遥测格式化：状态快照 → 一行文本。
 *
 * 纯计算，不含 HAL、不含串口。app 组好快照后调 telemetry_format() 拿到一行文本，
 * 自己决定发给谁（串口 / 屏幕 / 日志）。
 *
 * ── 两种格式 ─────────────────────────────────────────────────────────
 *   HUMAN —— 给人看：字段带名字，扫一眼就知道是什么。
 *   CSV   —— 给机器看：固定前缀 `@TEL,` + 固定字段顺序，用一行表头描述，
 *            方便上位机 `grep '^@TEL,'` 一键抽取、直接喂给绘图脚本。
 *
 *   两者**共用同一份快照**，所以不可能出现「屏幕上和导出文件对不上」。
 *
 * ── 为什么全整数、不用 %f ────────────────────────────────────────────
 *   newlib-nano 默认不带浮点格式化（要用得加 `-u _printf_float`，会把几 KB
 *   代码拉进来）。温度本来就是整数毫摄氏度，直接用整数拼小数最省，
 *   也保证任何平台上输出逐字节一致——这对「拿日志做回归对比」很重要。
 * ────────────────────────────────────────────────────────────────────
 */
#ifndef MODULE_TELEMETRY_H
#define MODULE_TELEMETRY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "interface/error.h"
#include "module/common/control_types.h"

/** 遥测输出格式。 */
typedef enum {
    TELEMETRY_FMT_HUMAN = 0,   /**< 人类可读 */
    TELEMETRY_FMT_CSV,         /**< `@TEL,` 前缀的定长字段行 */
} telemetry_format_t;

/** 一帧遥测的全部输入。纯值，无指针，可整体复制/回放。 */
typedef struct {
    uint16_t    seq;           /**< 帧序号（回绕），用于检测丢帧 */
    uint32_t    tick_ms;       /**< 设备时间轴 */
    fan_mode_t  mode;
    uint8_t     duty_percent;
    int32_t     temp_milli_c;  /**< 毫摄氏度；temp_valid 为假时该值无意义 */
    bool        temp_valid;
    uint16_t    adc_raw;       /**< ADC 原始计数（不经 NTC 换算） */
    uint32_t    rpm;           /**< 实测转速（观测单位） */
    uint32_t    target_rpm;    /**< 内环目标转速 */
    fan_alarm_t alarms;
} telemetry_sample_t;

/**
 * @brief 把一帧格式化成一行文本。
 *
 * @param sample    采样快照。
 * @param format    格式。
 * @param buf       输出缓冲。
 * @param cap       缓冲容量（字节）。
 * @param out_len   输出，写入的字节数（不含结尾 NUL）。
 *
 * @return error_t ERR_OK 成功；
 *                 ERR_INVALID_PARAM 空指针或 cap 为 0；
 *                 ERR_OVERFLOW 缓冲不够（**内容已被截断**，out_len 是实际写入长度）。
 *
 * @note 溢出**必须**返回错误而不是静默截断：被截断的遥测帧会丢掉尾部的字段
 *       （恰好是报警位），而绘制出来的曲线看起来完全正常。
 */
error_t telemetry_format(const telemetry_sample_t *sample, telemetry_format_t format,
                         char *buf, size_t cap, size_t *out_len);

/**
 * @brief CSV 格式的表头（以 `#` 开头，绘图脚本按注释跳过）。
 *
 * @return const char* 静态字面量。
 */
const char *telemetry_csv_header(void);

#endif /* MODULE_TELEMETRY_H */
