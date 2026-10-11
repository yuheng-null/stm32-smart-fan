/**
 * @file app_display.c
 * @brief OLED 显示的实现。
 *
 * ── 为什么要有「内容没变就不刷」这一层 ───────────────────────────────
 *   ssd1306_update_screen() 会把本地 1 KB 显存经软件 I2C 整块推给屏幕。
 *   软件 I2C 是位翻转，1 KB 大约几十毫秒——这是主循环里最大的一块阻塞。
 *   而稳态下（温度刚变化、转速刚稳定）连着好几帧内容**完全一样**，
 *   这时候推的 1 KB 是纯浪费。做一次 memcmp 就能省掉它。
 *
 *   注意：这不是「为了跑得快的偷工减料」，而是让控制节拍的抖动变小——
 *   真正的补偿手段是 thermal_control 用**真实 dt**。
 * ────────────────────────────────────────────────────────────────────
 */

#include "app/app_display.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "app/app_control.h"

#include "bsp/bsp_oled.h"

#include "driver/device/ssd1306/ssd1306.h"
#include "driver/device/ssd1306/ssd1306_fonts.h"

#include "module/display_view/display_view.h"

/* --------------------------------------------------------------- 布局 */

/** 6×8 字库的字符步进（像素）。 */
#define APP_GLYPH_STEP_6X8      (6U)

/** 11×18 字库的字符步进（像素）。 */
#define APP_GLYPH_STEP_11X18    (11U)

/** 屏幕宽度（像素）。 */
#define APP_SCREEN_WIDTH        (128U)

/** 6×8 文本行之间的垂直间距（像素）。8 像素高的字留 3 像素行距。 */
#define APP_TEXT_ROW_STEP       (11U)

/** 第一行 6×8 文本的 y 坐标：让出大字号温度那一行。 */
#define APP_TEXT_ROW0_Y         (22U)

/* --------------------------------------------------------------- 状态 */

/** OLED 设备实例。显存 1 KB，必须静态存储期，不能放栈上。 */
static ssd1306_t s_oled;

static bool           s_present    = false;
static display_view_t s_last_view;
static bool           s_have_last  = false;

/* ----------------------------------------------------------------- 内部 */

/** @brief 把一帧视图画进显存并上屏。 */
static void app_display_draw(const display_view_t *view)
{
    (void)ssd1306_fill(&s_oled, SSD1306_BLACK);

    /* 大字号温度，左上角 */
    (void)ssd1306_set_cursor(&s_oled, 0U, 0U);
    (void)ssd1306_write_string(&s_oled, view->temp_text, &Font_11x18, SSD1306_WHITE);

    /* 单位紧跟在数值后面，位置随数值长度浮动——比固定位置更省横向空间 */
    const uint16_t unit_x = (uint16_t)(strlen(view->temp_text) * APP_GLYPH_STEP_11X18);
    (void)ssd1306_set_cursor(&s_oled, unit_x, 4U);
    (void)ssd1306_write_string(&s_oled, "C", &Font_11x18, SSD1306_WHITE);

    /* 模式短码靠右对齐，放大字号那一行的右侧 */
    const uint16_t mode_w = (uint16_t)(strlen(view->mode_text) * APP_GLYPH_STEP_6X8);
    const uint16_t mode_x = (mode_w < APP_SCREEN_WIDTH) ? (uint16_t)(APP_SCREEN_WIDTH - mode_w)
                                                        : 0U;

    (void)ssd1306_set_cursor(&s_oled, mode_x, 6U);
    (void)ssd1306_write_string(&s_oled, view->mode_text, &Font_6x8, SSD1306_WHITE);

    /* 其余文本行 */
    for (uint8_t i = 0U; i < view->line_count; i++) {
        (void)ssd1306_set_cursor(&s_oled, 0U,
                                 (uint16_t)(APP_TEXT_ROW0_Y + ((uint16_t)i * APP_TEXT_ROW_STEP)));
        (void)ssd1306_write_string(&s_oled, view->lines[i], &Font_6x8, SSD1306_WHITE);
    }

    (void)ssd1306_update_screen(&s_oled);
}

/* ----------------------------------------------------------------- 对外 */

error_t app_display_init(void)
{
    s_have_last = false;
    s_present   = (bsp_oled_init(&s_oled) == ERR_OK);

    return s_present ? ERR_OK : ERR_COMMUNICATION;
}

void app_display_task(void)
{
    if (!s_present) {
        return;
    }

    app_status_t status;

    if (!app_control_get_status(&status)) {
        return;
    }

    const display_view_input_t input = {
        .mode                = status.mode,
        .temp_valid          = status.temp_valid,
        .temp_milli_c        = status.temp_milli_c,
        .rpm_valid           = status.rpm_valid,
        .rpm                 = status.rpm,
        .duty_percent        = status.duty_percent,
        .target_rpm          = status.target_rpm,
        .target_temp_milli_c = status.target_temp_milli_c,
        .manual_duty_percent = status.manual_duty_percent,
        .alarms              = status.alarms,
    };

    display_view_t view;

    if (display_view_build(&input, &view) != ERR_OK) {
        return;
    }

    /* 内容与上一帧逐字节相同 → 这次上屏没有意义，直接跳过。 */
    if (s_have_last && (memcmp(&view, &s_last_view, sizeof(view)) == 0)) {
        return;
    }

    app_display_draw(&view);

    s_last_view = view;
    s_have_last = true;
}

bool app_display_is_present(void)
{
    return s_present;
}
