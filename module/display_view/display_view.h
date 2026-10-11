/**
 * @file display_view.h
 * @brief OLED 视图模型：运行状态 → 几行文本。
 *
 * 纯计算，不含 HAL、不 include ssd1306。app 拿到这些文本自己去画。
 *
 * ── 为什么这件事值得单独一个模块 ─────────────────────────────────────
 *   屏幕是 128×64，6×8 字库下**每行只能放 21 个字符**。而
 *   `ssd1306_write_string()` 超宽时**不报错**——它只是把多余的字符画到看不见的
 *   地方。本工程早先就踩过一次（README 有记录）。
 *   把「一行文本怎么拼」放进可单测的模块，这条约束就能用一条断言钉死
 *   （tests/test_display_view.c 的 test_every_line_fits_the_screen），
 *   而不是靠在硬件上盯着屏幕找。
 *
 * ── 视图内容的取舍 ───────────────────────────────────────────────────
 *   屏幕小，只放**现在要用的**：
 *     · 大字号温度（唯一一眼要看到的量）
 *     · 右上角模式短码
 *     · 目标温度 / 当前档位（用户按 K1/K2 改的就是它）
 *     · duty 与实测转速（判断闭环有没有在工作的最直接证据）
 *     · 目标转速与误差（整定时看）
 *     · 报警短码（只在有报警时才占一行）
 *   原始 ADC 与 NTC 阻值不上面——它们是**标定**用的，串口那行有。
 * ────────────────────────────────────────────────────────────────────
 */
#ifndef MODULE_DISPLAY_VIEW_H
#define MODULE_DISPLAY_VIEW_H

#include <stdbool.h>
#include <stdint.h>

#include "interface/error.h"
#include "module/common/control_types.h"

/** 6×8 字库下，一行最多能放几个字符（128 / 6 = 21）。 */
#define DISPLAY_VIEW_LINE_CHARS (22U)   /**< 21 个字符 + 结尾 NUL */

/** 大字号温度行之外的文本行数。 */
#define DISPLAY_VIEW_MAX_LINES  (4U)

/** 视图输入。 */
typedef struct {
    fan_mode_t  mode;
    bool        temp_valid;
    int32_t     temp_milli_c;
    bool        rpm_valid;
    uint32_t    rpm;
    uint8_t     duty_percent;
    uint32_t    target_rpm;
    uint32_t    target_temp_milli_c;   /**< 当前设定温度（曲线起步点 + 偏移） */
    uint8_t     manual_duty_percent;
    fan_alarm_t alarms;
} display_view_input_t;

/** 视图输出。 */
typedef struct {
    char    temp_text[10];   /**< 大字号显示的数值，如 "23.8"；无效时 "---" */
    char    mode_text[8];    /**< 右上角模式短码 */
    char    lines[DISPLAY_VIEW_MAX_LINES][DISPLAY_VIEW_LINE_CHARS];
    uint8_t line_count;      /**< lines 里有效行数（报警行为空时少一行） */
} display_view_t;

/**
 * @brief 由状态构建视图。
 *
 * @param in  状态输入。
 * @param out 视图输出。
 *
 * @return error_t ERR_OK；ERR_INVALID_PARAM 空指针。
 *
 * @note 保证每行不超过 DISPLAY_VIEW_LINE_CHARS − 1 个字符；温度无效时用 "---"
 *       占位，绝不显示一个看似正常的数。
 */
error_t display_view_build(const display_view_input_t *in, display_view_t *out);

#endif /* MODULE_DISPLAY_VIEW_H */
