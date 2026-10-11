/**
 * @file app_display.c
 * @brief OLED 显示的实现。
 *
 * ── 为什么要有「内容没变就不刷」这一层 ───────────────────────────────
 *   bsp_oled_flush() 会把本地 1 KB 显存经软件 I2C 整块推给屏幕。
 *   软件 I2C 是位翻转，1 KB 大约几十毫秒——这是主循环里最大的一块阻塞。
 *   而稳态下（温度刚变化、转速刚稳定）连着好几帧内容**完全一样**，
 *   这时候推的 1 KB 是纯浪费。做一次 memcmp 就能省掉它。
 *
 *   注意：这不是「为了跑得快的偷工减料」，而是让控制节拍的抖动变小——
 *   真正的补偿手段是 thermal_control 用**真实 dt**。
 *
 * ── 本文件不认识 SSD1306 ─────────────────────────────────────────────
 *   排版在这里，但画字与上屏走 bsp/bsp_oled.h 的接口（bsp_oled_draw_text /
 *   bsp_oled_text_width / bsp_oled_flush）。APP 层不许放外设驱动
 *   （见 docs/architecture.md），所以驱动类型与实例都关在 BSP 里，
 *   换屏幕时本文件一行不用改。
 * ────────────────────────────────────────────────────────────────────
 */

#include "app/app_display.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "app/app_control.h"

#include "bsp/bsp_oled.h"

#include "module/display_view/display_view.h"

/* --------------------------------------------------------------- 布局 */

/** 6×8 文本行之间的垂直间距（像素）。8 像素高的字留 3 像素行距。 */
#define APP_TEXT_ROW_STEP       (11U)

/** 第一行 6×8 文本的 y 坐标：让出大字号温度那一行。 */
#define APP_TEXT_ROW0_Y         (22U)

/** 大字号（11×18）内部，把「单位 C」略微下移几像素，与数字视觉居中对齐。 */
#define APP_UNIT_Y              (4U)

/** 模式短码（6×8）放在大字号那一行的右上角时的 y 坐标。 */
#define APP_MODE_Y              (6U)

/* --------------------------------------------------------------- 状态 */

static bool           s_present    = false;
static display_view_t s_last_view;
static bool           s_have_last  = false;

/* ----------------------------------------------------------------- 内部 */

/** @brief 把一帧视图画进显存并上屏。 */
static void app_display_draw(const display_view_t *view)
{
    bsp_oled_clear();

    /* 大字号温度，左上角 */
    bsp_oled_draw_text(0U, 0U, view->temp_text, BSP_OLED_FONT_LARGE);

    /* 单位紧跟在数值后面，位置随数值长度浮动——比固定位置更省横向空间 */
    const uint16_t unit_x = bsp_oled_text_width(view->temp_text, BSP_OLED_FONT_LARGE);
    bsp_oled_draw_text(unit_x, APP_UNIT_Y, "C", BSP_OLED_FONT_LARGE);

    /* 模式短码靠右对齐，放大字号那一行的右侧 */
    const uint16_t mode_w = bsp_oled_text_width(view->mode_text, BSP_OLED_FONT_SMALL);
    const uint16_t mode_x = (mode_w < BSP_OLED_WIDTH_PX)
                                ? (uint16_t)(BSP_OLED_WIDTH_PX - mode_w)
                                : 0U;

    bsp_oled_draw_text(mode_x, APP_MODE_Y, view->mode_text, BSP_OLED_FONT_SMALL);

    /* 其余文本行 */
    for (uint8_t i = 0U; i < view->line_count; i++) {
        bsp_oled_draw_text(0U,
                           (uint16_t)(APP_TEXT_ROW0_Y + ((uint16_t)i * APP_TEXT_ROW_STEP)),
                           view->lines[i], BSP_OLED_FONT_SMALL);
    }

    bsp_oled_flush();
}

/* ----------------------------------------------------------------- 对外 */

error_t app_display_init(void)
{
    s_have_last = false;
    s_present   = (bsp_oled_init() == ERR_OK);

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
