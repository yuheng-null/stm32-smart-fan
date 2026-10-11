/**
 * @file app_main.c
 * @brief 应用层入口：OLED + NTC + 按键的驱动测试台。
 *
 * 本层严禁出现 HAL 调用（见 docs/architecture.md 的依赖规则），
 * 所有硬件动作都经 bsp/ 与 driver/ 暴露的接口完成。
 */

#include "app/app_main.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "bsp/bsp_adc.h"
#include "bsp/bsp_i2c.h"
#include "bsp/bsp_key.h"
#include "bsp/bsp_oled.h"
#include "bsp/bsp_system.h"
#include "bsp/bsp_uart.h"

#include "driver/device/ssd1306/ssd1306.h"
#include "driver/device/ssd1306/ssd1306_fonts.h"

#include "interface/error.h"
#include "interface/uart_if.h"

#include "module/temperature/temperature.h"

/* ----------------------------------------------------------------- 配置 */

/** 状态刷新周期（毫秒）：串口打印与 OLED 刷新共用这个节拍。 */
#define APP_REPORT_PERIOD_MS    (200U)

/** 温度采样时对 ADC 求平均的次数。 */
#define APP_ADC_AVERAGE_COUNT   (16U)

/** 单次发送的阻塞上限。115200 下发几十字节只需几百微秒，200 ms 很宽松。 */
#define APP_TX_TIMEOUT_MS       (200U)

/** 一行文本的缓冲。最长的是状态行，约 110 字节，留足余量。 */
#define APP_LINE_BUFFER_SIZE    (160U)

/** 一次从接收缓冲取走的字节数上限。 */
#define APP_RX_CHUNK_SIZE       (64U)

/** 总线扫描最多记录多少个地址。 */
#define APP_SCAN_MAX            (8U)

/** 显示「无有效示数」时用的占位串。 */
#define APP_NO_VALUE            "---"

/* --------------------------------------------------------------- 静态量 */

/** 调试串口契约。app_main_init() 里绑定，之后只读。 */
static const uart_if_t *s_uart;

/** OLED 设备实例。显存 1 KB，必须静态存储期，不能放栈上。 */
static ssd1306_t s_oled;

/** 屏幕是否可用。不可用时跳过所有刷新，但温度与按键的测试照常进行。 */
static bool s_oled_ok = false;

/** 两个按键各自被按下的累计次数（边沿计数，用来看消抖有没有多计/漏计）。 */
static uint32_t s_key_press_count[KEY_ID_COUNT];

/* ----------------------------------------------------------- 串口小工具 */

/**
 * @brief 经契约发送一段文本。
 *
 * @param text 待发送文本，不得为 NULL。
 * @param len  字节数，不得为 0。
 *
 * @return error_t 见 uart_write_fn 的约定；未初始化时返回 ERR_NOT_INITIALIZED。
 */
static error_t app_write_text(const char *text, uint32_t len)
{
    if (s_uart == NULL) {
        return ERR_NOT_INITIALIZED;
    }
    return s_uart->write(s_uart->ctx, (const uint8_t *)text, len, APP_TX_TIMEOUT_MS);
}

/**
 * @brief 发送一段编译期已知长度的静态文字（不复制、不缓冲）。
 */
static error_t app_send_literal(const char *text, size_t len)
{
    if (len == 0U) {
        return ERR_INVALID_PARAM;
    }
    return app_write_text(text, (uint32_t)len);
}

/**
 * @brief 把 snprintf 的结果发出去；编码失败或被截断则不发。
 *
 * @param len snprintf 的返回值；为负表示编码错误。
 * @param cap 缓冲区容量，用于识别截断。
 */
static error_t app_send_formatted(const char *text, int len, size_t cap)
{
    if ((len <= 0) || ((size_t)len >= cap)) {
        return ERR_INVALID_PARAM;
    }
    return app_write_text(text, (uint32_t)len);
}

/* --------------------------------------------------------------- 格式化 */

/**
 * @brief 把毫摄氏度格式化成文本，如 "25.4" / "-3.1"。
 *
 * 刻意不用 printf 的 %f：newlib-nano 默认不带浮点格式化，
 * 要用得加链接选项 `-u _printf_float`，会把几 KB 的代码拉进来。
 * 毫摄氏度本来就是整数，直接用整数除余拼出来最省。
 *
 * @param milli_c 毫摄氏度。
 * @param buf     输出缓冲。
 * @param cap     缓冲容量。
 */
static void app_format_celsius(int32_t milli_c, char *buf, size_t cap)
{
    const char *sign  = "";
    int32_t     value = milli_c;

    if (value < 0) {
        sign  = "-";
        value = -value;
    }

    /* 四舍五入到 0.1 ℃：先加半个步长再整除。 */
    const int32_t tenths = (value + 50) / 100;

    if (tenths == 0) {
        sign = "";      /* 避免出现 "-0.0" */
    }

    (void)snprintf(buf, cap, "%s%ld.%ld", sign,
                   (long)(tenths / 10), (long)(tenths % 10));
}

/**
 * @brief 把欧姆格式化成 kΩ 文本，如 "10.03"。
 */
static void app_format_kohm(uint32_t r_ohm, char *buf, size_t cap)
{
    (void)snprintf(buf, cap, "%lu.%02lu",
                   (unsigned long)(r_ohm / 1000U),
                   (unsigned long)((r_ohm % 1000U) / 10U));
}

/* ----------------------------------------------------------- 开机自检 */

/**
 * @brief 扫描 I2C 总线并把结果报到串口。
 *
 * 这一步是排障的关键：屏不亮时，它能把三种原因分开——
 *   · 一个地址都没有 → 接线/上拉/供电问题；
 *   · 只有 0x3C 或 0x3D → 正常；
 *   · 出现别的地址   → 总线上挂了非预期器件。
 */
static void app_report_bus_scan(void)
{
    uint8_t       found[APP_SCAN_MAX];
    char          line[APP_LINE_BUFFER_SIZE];
    const uint8_t count = bsp_oled_bus_scan(found, (uint8_t)APP_SCAN_MAX);

    int len = snprintf(line, sizeof(line), "I2C scan: %u device(s)", (unsigned)count);
    (void)app_send_formatted(line, len, sizeof(line));

    for (uint8_t i = 0U; (i < count) && (i < (uint8_t)APP_SCAN_MAX); i++) {
        len = snprintf(line, sizeof(line), " 0x%02X", (unsigned)found[i]);
        (void)app_send_formatted(line, len, sizeof(line));
    }

    (void)app_send_literal("\r\n", 2U);
}

/**
 * @brief 开机横幅：静态文字 + 一行动态主频。
 *
 * 横幅里有中文，UTF-8 下一个汉字 3 字节。静态文字直接以字面量发送，
 * 既不占 RAM 也根本不会被 snprintf 静默截断。
 */
static error_t app_send_banner(void)
{
    static const char banner_head[] = "\r\n==== smart_fan boot ====\r\n";

    static const char banner_tail[] =
        "外设: OLED(SSD1306, 软 I2C PB6/PB7) + NTC(PA0/ADC1_IN0) + 按键(PC0/PC1)\r\n"
        "上面这几行在串口助手里不是乱码，就说明时钟树与波特率都对。\r\n"
        "温度是对 NTC 分压做的换算，参数尚未校准，只看趋势不看绝对值。\r\n"
        "\r\n";

    char    line[APP_LINE_BUFFER_SIZE];
    error_t ret;

    ret = app_send_literal(banner_head, sizeof(banner_head) - 1U);
    if (ret != ERR_OK) {
        return ret;
    }

    const int len = snprintf(line, sizeof(line), "SYSCLK = %lu Hz\r\n",
                             (unsigned long)bsp_system_get_sysclk_hz());
    ret = app_send_formatted(line, len, sizeof(line));
    if (ret != ERR_OK) {
        return ret;
    }

    return app_send_literal(banner_tail, sizeof(banner_tail) - 1U);
}

/* --------------------------------------------------------------- 刷新 */

/**
 * @brief 刷新 OLED。
 *
 * 先把所有内容画进本地显存，最后一次性 update_screen 上屏——
 * 若每画一行就上屏，屏幕会逐行闪。
 *
 * @param temp_text 已格式化的温度文本。
 * @param r_text    已格式化的阻值文本。
 * @param raw       ADC 原始计数。
 * @param temp_ok   温度是否落在有效量程内。
 */
static void app_oled_refresh(const char *temp_text, const char *r_text,
                             uint16_t raw, bool temp_ok)
{
    char line[APP_LINE_BUFFER_SIZE];

    if (!s_oled_ok) {
        return;
    }

    ssd1306_fill(&s_oled, SSD1306_BLACK);

    /* 第一行：温度，用最大的字号 */
    (void)ssd1306_set_cursor(&s_oled, 0U, 0U);
    (void)ssd1306_write_string(&s_oled, temp_ok ? temp_text : APP_NO_VALUE,
                               &Font_11x18, SSD1306_WHITE);
    (void)ssd1306_set_cursor(&s_oled, 96U, 4U);
    (void)ssd1306_write_string(&s_oled, "C", &Font_11x18, SSD1306_WHITE);

    /* 第二、三行：中间量，排障时对得上号 */
    (void)ssd1306_set_cursor(&s_oled, 0U, 22U);
    (void)snprintf(line, sizeof(line), "raw %u", (unsigned)raw);
    (void)ssd1306_write_string(&s_oled, line, &Font_7x10, SSD1306_WHITE);

    (void)ssd1306_set_cursor(&s_oled, 0U, 34U);
    (void)snprintf(line, sizeof(line), "R %s kohm", r_text);
    (void)ssd1306_write_string(&s_oled, line, &Font_7x10, SSD1306_WHITE);

    /* 第四行：两个按键的当前状态与累计按下次数 */
    (void)ssd1306_set_cursor(&s_oled, 0U, 46U);
    (void)snprintf(line, sizeof(line), "K1%c n=%lu   K2%c n=%lu",
                   bsp_key_is_pressed(KEY_ID_K1) ? '*' : '-',
                   (unsigned long)s_key_press_count[KEY_ID_K1],
                   bsp_key_is_pressed(KEY_ID_K2) ? '*' : '-',
                   (unsigned long)s_key_press_count[KEY_ID_K2]);
    (void)ssd1306_write_string(&s_oled, line, &Font_7x10, SSD1306_WHITE);

    (void)ssd1306_update_screen(&s_oled);
}

/**
 * @brief 把一行状态报到串口。
 */
static void app_report_status(const char *temp_text, const char *r_text,
                              uint16_t raw, bool temp_ok, error_t temp_status)
{
    char line[APP_LINE_BUFFER_SIZE];

    const int len = snprintf(
        line, sizeof(line),
        "[%5lu ms] T = %s C  raw = %4u  R = %s kohm  K1 = %lu(%c)  K2 = %lu(%c)  "
        "drop = %lu  err = %lu  status = %d\r\n",
        (unsigned long)bsp_system_get_tick_ms(),
        temp_ok ? temp_text : APP_NO_VALUE,
        (unsigned)raw,
        r_text,
        (unsigned long)s_key_press_count[KEY_ID_K1],
        bsp_key_is_pressed(KEY_ID_K1) ? '*' : '-',
        (unsigned long)s_key_press_count[KEY_ID_K2],
        bsp_key_is_pressed(KEY_ID_K2) ? '*' : '-',
        (unsigned long)bsp_uart_get_rx_drop_count(),
        (unsigned long)bsp_uart_get_rx_error_count(),
        (int)temp_status);

    (void)app_send_formatted(line, len, sizeof(line));
}

/* --------------------------------------------------------------- 对外 */

error_t app_main_init(void)
{
    error_t ret;

    s_uart = bsp_uart_get();
    if (s_uart == NULL) {
        return ERR_NOT_INITIALIZED;
    }

    /* 打开接收中断。漏了这一步 read() 永远返回 0——而且不报任何错。 */
    ret = bsp_uart_init();
    if (ret != ERR_OK) {
        return ret;
    }

    ret = bsp_adc_init();
    if (ret != ERR_OK) {
        return ret;
    }

    ret = bsp_key_init();
    if (ret != ERR_OK) {
        return ret;
    }

    ret = bsp_i2c_init();
    if (ret != ERR_OK) {
        return ret;
    }

    /*
     * OLED 失败不算致命：屏没接好时，温度与按键的测试照样能做，
     * 故障交给下面开机时的总线扫描报出来，不在这里中止。
     */
    s_oled_ok = (bsp_oled_init(&s_oled) == ERR_OK);

    for (uint8_t i = 0U; i < (uint8_t)KEY_ID_COUNT; i++) {
        s_key_press_count[i] = 0U;
    }

    return ERR_OK;
}

void app_main_run(void)
{
    uint8_t  rx_chunk[APP_RX_CHUNK_SIZE];
    uint32_t next_report_ms = 0U;
    bool     banner_done    = false;

    for (;;) {
        /* ---- 开机横幅与总线自检：发不出去就下一轮再试 ---- */
        if (!banner_done) {
            if (app_send_banner() != ERR_OK) {
                continue;
            }

            static const char oled_line[] = "OLED: probing 0x3C/0x3D ...\r\n";
            (void)app_send_literal(oled_line, sizeof(oled_line) - 1U);
            app_report_bus_scan();

            static const char oled_ok_msg[]   = "OLED: OK\r\n";
            static const char oled_fail_msg[] = "OLED: 无应答——检查 VCC/GND/SCL/SDA 与上拉\r\n";
            (void)app_send_literal(s_oled_ok ? oled_ok_msg : oled_fail_msg,
                                   s_oled_ok ? (sizeof(oled_ok_msg) - 1U)
                                             : (sizeof(oled_fail_msg) - 1U));

            banner_done    = true;
            next_report_ms = bsp_system_get_tick_ms() + APP_REPORT_PERIOD_MS;
            continue;
        }

        /* ---- 按键：事件是边沿，读取即清除 ---- */
        for (uint8_t i = 0U; i < (uint8_t)KEY_ID_COUNT; i++) {
            if (bsp_key_take_event((key_id_t)i) == KEY_EVENT_PRESS) {
                s_key_press_count[i]++;
            }
        }

        /* ---- 串口回显：一次整块取走，收发时序与主循环解耦 ---- */
        const uint32_t got = s_uart->read(s_uart->ctx, rx_chunk, (uint32_t)sizeof(rx_chunk));
        if (got > 0U) {
            (void)app_write_text((const char *)rx_chunk, got);
        }

        /* ---- 周期上报 ---- */
        /* 用无符号减法判到期：tick 回绕时仍然正确，直接比较大小则不行。 */
        if ((int32_t)(bsp_system_get_tick_ms() - next_report_ms) >= 0) {
            uint16_t raw        = 0U;
            int32_t  milli_c    = 0;
            char     temp_text[16] = APP_NO_VALUE;
            char     r_text[16]    = "0.00";
            bool     temp_ok    = false;
            error_t  temp_status = ERR_NOT_INITIALIZED;

            next_report_ms += APP_REPORT_PERIOD_MS;

            if (bsp_adc_read_averaged(&raw, (uint16_t)APP_ADC_AVERAGE_COUNT) == ERR_OK) {
                const uint32_t r_ohm = temperature_resistance_from_raw(raw);

                app_format_kohm(r_ohm, r_text, sizeof(r_text));

                temp_status = temperature_from_adc_raw(raw, &milli_c);
                if (temp_status == ERR_OK) {
                    temp_ok = true;
                    app_format_celsius(milli_c, temp_text, sizeof(temp_text));
                }
            }

            app_oled_refresh(temp_text, r_text, raw, temp_ok);
            app_report_status(temp_text, r_text, raw, temp_ok, temp_status);
        }
    }
}
