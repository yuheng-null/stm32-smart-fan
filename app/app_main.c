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
#include "bsp/bsp_buzzer.h"
#include "bsp/bsp_fan.h"
#include "bsp/bsp_i2c.h"
#include "bsp/bsp_key.h"
#include "bsp/bsp_oled.h"
#include "bsp/bsp_relay.h"
#include "bsp/bsp_system.h"
#include "bsp/bsp_uart.h"

#include "driver/device/ssd1306/ssd1306.h"
#include "driver/device/ssd1306/ssd1306_fonts.h"

#include "interface/error.h"
#include "interface/uart_if.h"

#include "module/fan_control/fan_control.h"
#include "module/temperature/temperature.h"

/* ----------------------------------------------------------------- 配置 */

/** 状态刷新周期（毫秒）：串口打印与 OLED 刷新共用这个节拍。 */
#define APP_REPORT_PERIOD_MS    (200U)

/** 温度采样时对 ADC 求平均的次数。 */
#define APP_ADC_AVERAGE_COUNT   (16U)

/** 单次发送的阻塞上限。115200 下发几十字节只需几百微秒，200 ms 很宽松。 */
#define APP_TX_TIMEOUT_MS       (200U)

/**
 * 一行文本的缓冲。
 *
 * 最长的是状态行：温度那段约 110 字节，加上风扇/继电器/蜂鸣器那段约 70 字节，
 * 留到 256 有充分余量。**改状态行格式时记得回来核对这里**——
 * snprintf 截断是静默的，只会把行尾悄悄吃掉。
 */
#define APP_LINE_BUFFER_SIZE    (256U)

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

/* ------------------------------------------------- 风扇 / 继电器 / 蜂鸣器 */

/**
 * @brief 取当前风扇转速。
 *
 * @param status 输出，转速换算的返回码（调试用，能区分「没信号」「太慢」
 *               「还没凑够两个沿」——转速一律为 0，成因却不同）。
 *
 * @return uint32_t 转速（转/分）；取不到时为 0。
 */
static uint32_t app_fan_rpm(error_t *status)
{
    uint32_t period_us = 0U;
    uint32_t rpm       = 0U;
    error_t  err       = bsp_fan_get_period_us(&period_us);

    if (err == ERR_OK) {
        err = fan_control_rpm_from_period_us(period_us, &rpm);
    }

    if (err != ERR_OK) {
        rpm = 0U;       /* 没信号 / 太慢 / 算不出，一律按 0 报，成因看 status */
    }

    if (status != NULL) {
        *status = err;
    }

    return rpm;
}

/**
 * @brief 把风扇、继电器、蜂鸣器的当前状态打一行到串口。
 *
 * 三个模块都印在同一行，是为了让「按一下键 → 这一行立刻变」这件事
 * 一眼可见；分散在多行反而要在滚动里找。
 */
static void app_report_devices(void)
{
    char          line[APP_LINE_BUFFER_SIZE];
    error_t       rpm_status = ERR_NOT_INITIALIZED;
    const uint32_t rpm       = app_fan_rpm(&rpm_status);

    const int len = snprintf(
        line, sizeof(line),
        "FAN: duty = %u%%  rpm = %lu  fg = %lu  rpm_status = %d"
        "  | RELAY = %s  | BUZZER = %s\r\n",
        (unsigned)bsp_fan_get_duty_percent(),
        (unsigned long)rpm,
        (unsigned long)bsp_fan_get_capture_count(),
        (int)rpm_status,
        bsp_relay_is_on() ? "ON" : "OFF",
        bsp_buzzer_is_on() ? "ON" : "OFF");

    (void)app_send_formatted(line, len, sizeof(line));
}

/**
 * @brief 占空比加/减一档，并立刻上报。
 *
 * 夹取与步进规则在 module/fan_control 里（纯计算，可单测），
 * 这里只负责「取当前值 → 算下一档 → 写硬件 → 报出来」。
 */
static void app_fan_step_duty(fan_duty_dir_t dir)
{
    static const char set_failed[] = "FAN: 设置占空比失败\r\n";

    const uint8_t next = fan_control_duty_step(bsp_fan_get_duty_percent(), dir);

    if (bsp_fan_set_duty_percent(next) != ERR_OK) {
        (void)app_send_literal(set_failed, sizeof(set_failed) - 1U);
        return;
    }

    app_report_devices();
}

/**
 * @brief 诊断：绕过定时器强制拉低某根引脚，并说明该怎么读结果。
 *
 * 见 bsp_fan.h 里 bsp_fan_force_pin_low() 的完整说明。
 */
static void app_fan_force(bsp_fan_force_t which)
{
    static const char note_pwm[] =
        "诊断: 已把 PA8（PWM 脚）强制拉低，绕过定时器。\r\n"
        "  风扇停 -> PA8 确实就是 PWM 线，问题在固件侧；\r\n"
        "  风扇照转 -> PA8 不是 PWM 线，试试 1（拉低 PA1）。\r\n";
    static const char note_tach[] =
        "诊断: 已把 PA1（测速脚）强制拉低，绕过定时器。\r\n"
        "  风扇停 -> PA1 上挂的才是 PWM 线 —— 两根信号线接反了。\r\n";
    static const char note_both[] =
        "诊断: 已把 PA8 与 PA1 都强制拉低。\r\n";
    static const char note_tail[] = "  复位板子即可恢复正常控制。\r\n";

    bsp_fan_force_pin_low(which);

    if (which == BSP_FAN_FORCE_PWM) {
        (void)app_send_literal(note_pwm, sizeof(note_pwm) - 1U);
    } else if (which == BSP_FAN_FORCE_TACH) {
        (void)app_send_literal(note_tach, sizeof(note_tach) - 1U);
    } else {
        (void)app_send_literal(note_both, sizeof(note_both) - 1U);
    }

    (void)app_send_literal(note_tail, sizeof(note_tail) - 1U);
    app_report_devices();
}

/** 串口命令表。用字面量发送，不占 RAM、也不会被 snprintf 截断。 */
static const char APP_HELP[] =
    "\r\n命令（单字符，大小写均可）:\r\n"
    "  + / -   风扇占空比 +10% / -10%\r\n"
    "  r       继电器吸合/断开（切换）\r\n"
    "  b       蜂鸣器响/停（切换）\r\n"
    "  t       立刻打印一行风扇/继电器/蜂鸣器状态\r\n"
    "  p       诊断：绕过定时器，把 PA8/PA1 都强制拉低（复位板子恢复）\r\n"
    "  8       诊断：只强制拉低 PA8   1：只强制拉低 PA1\r\n"
    "  ?       显示本表\r\n"
    "另外：K1 加档、K2 减档（与 + / - 等效）。\r\n\r\n";

/**
 * @brief 处理串口收到的一批字节里的单字符命令。
 *
 * 为什么继电器和蜂鸣器走串口而不是再加按键：板上只有两个键，已经给了
 * 占空比加减；这两个器件需要能立刻手动触发，串口是现成的手段，
 * 而且比按键更容易确认「到底有没有执行」——命令本身就带着回显。
 *
 * 不认识的字符一律忽略：回显已经在主循环里做了，这里再报一次只会刷屏。
 */
static void app_handle_commands(const uint8_t *buf, uint32_t len)
{
    for (uint32_t i = 0U; i < len; i++) {
        switch ((char)buf[i]) {
        case '+':
            app_fan_step_duty(FAN_DUTY_UP);
            break;

        case '-':
            app_fan_step_duty(FAN_DUTY_DOWN);
            break;

        case 'r':
        case 'R':
            (void)bsp_relay_set(!bsp_relay_is_on());
            app_report_devices();
            break;

        case 'b':
        case 'B':
            (void)bsp_buzzer_set(!bsp_buzzer_is_on());
            app_report_devices();
            break;

        case 't':
        case 'T':
            app_report_devices();
            break;

        case '8':
            /* 诊断：只把 PA8 强制拉低（详见 bsp_fan.h）。执行后要复位才能恢复。 */
            app_fan_force(BSP_FAN_FORCE_PWM);
            break;

        case '1':
            /* 诊断：只把 PA1 强制拉低 */
            app_fan_force(BSP_FAN_FORCE_TACH);
            break;

        case 'p':
        case 'P':
            app_fan_force(BSP_FAN_FORCE_BOTH);
            break;

        case '?':
            (void)app_send_literal(APP_HELP, sizeof(APP_HELP) - 1U);
            break;

        default:
            break;
        }
    }
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
        "      + 风扇 PWM(PA8/TIM1_CH1, 25kHz, 开漏) + 风扇测速(PA1/TIM2_CH2)\r\n"
        "      + 继电器(PB0, 高电平触发) + 有源蜂鸣器(PB1, 低电平触发)\r\n"
        "上面这几行在串口助手里不是乱码，就说明时钟树与波特率都对。\r\n"
        "温度是对 NTC 分压做的换算，参数尚未校准，只看趋势不看绝对值。\r\n"
        "⚠️ 风扇必须在板子跑起来之后再给 12V：PWM 悬空 = 风扇内部上拉 = 全速。\r\n"
        "输入 ? 看串口命令表；K1 加档、K2 减档。\r\n"
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
    char          line[APP_LINE_BUFFER_SIZE];
    error_t       rpm_status = ERR_NOT_INITIALIZED;
    const uint32_t rpm       = app_fan_rpm(&rpm_status);

    (void)rpm_status;   /* 屏幕上不显示原因码，串口那一行有 */

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

    /*
     * 下面三行用 6x8 字库，不是 7x10。
     *
     * 一屏 128 像素：6 像素一个字 → 每行 21 个字符，7 像素一个字 → 只有 18。
     * 「duty 100%   rpm 30000」这种最坏情况正好 21 个字符，用 7x10 会被挤到
     * 屏幕外——而 write_string 越界**不报错**，只是画到看不见的地方，
     * 事后很难想到是字号的问题。
     */
    (void)ssd1306_set_cursor(&s_oled, 0U, 22U);
    (void)snprintf(line, sizeof(line), "duty %u%%   rpm %lu",
                   (unsigned)bsp_fan_get_duty_percent(), (unsigned long)rpm);
    (void)ssd1306_write_string(&s_oled, line, &Font_6x8, SSD1306_WHITE);

    /* 中间量：排障时和串口那一行对得上号（k 即 kΩ） */
    (void)ssd1306_set_cursor(&s_oled, 0U, 34U);
    (void)snprintf(line, sizeof(line), "raw %u  R %sk", (unsigned)raw, r_text);
    (void)ssd1306_write_string(&s_oled, line, &Font_6x8, SSD1306_WHITE);

    (void)ssd1306_set_cursor(&s_oled, 0U, 46U);
    (void)snprintf(line, sizeof(line), "RL %s  BZ %s",
                   bsp_relay_is_on() ? "ON" : "OFF",
                   bsp_buzzer_is_on() ? "ON" : "OFF");
    (void)ssd1306_write_string(&s_oled, line, &Font_6x8, SSD1306_WHITE);

    (void)ssd1306_update_screen(&s_oled);
}

/**
 * @brief 把一行状态报到串口。
 */
static void app_report_status(const char *temp_text, const char *r_text,
                              uint16_t raw, bool temp_ok, error_t temp_status)
{
    char          line[APP_LINE_BUFFER_SIZE];
    error_t       rpm_status = ERR_NOT_INITIALIZED;
    const uint32_t rpm       = app_fan_rpm(&rpm_status);

    const int len = snprintf(
        line, sizeof(line),
        "[%5lu ms] T = %s C  raw = %4u  R = %s kohm  K1 = %lu(%c)  K2 = %lu(%c)"
        "  | fan duty = %u%%  rpm = %lu  fg = %lu  rpm_status = %d"
        "  | relay = %s  buzz = %s"
        "  | drop = %lu  err = %lu  status = %d\r\n",
        (unsigned long)bsp_system_get_tick_ms(),
        temp_ok ? temp_text : APP_NO_VALUE,
        (unsigned)raw,
        r_text,
        (unsigned long)s_key_press_count[KEY_ID_K1],
        bsp_key_is_pressed(KEY_ID_K1) ? '*' : '-',
        (unsigned long)s_key_press_count[KEY_ID_K2],
        bsp_key_is_pressed(KEY_ID_K2) ? '*' : '-',
        (unsigned)bsp_fan_get_duty_percent(),
        (unsigned long)rpm,
        (unsigned long)bsp_fan_get_capture_count(),
        (int)rpm_status,
        bsp_relay_is_on() ? "ON" : "OFF",
        bsp_buzzer_is_on() ? "ON" : "OFF",
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
     * 继电器与蜂鸣器只是登记状态（引脚由 MX_GPIO_Init() 配好且已有初值），
     * 不会失败；写返回值判断是为了将来有人给它们加真硬件动作时不必回头补。
     */
    ret = bsp_relay_init();
    if (ret != ERR_OK) {
        return ret;
    }

    ret = bsp_buzzer_init();
    if (ret != ERR_OK) {
        return ret;
    }

    /*
     * 风扇：启动 25 kHz PWM（占空比 0，先停住）与 FG 捕获中断。
     *
     * ⚠️ 这一步**不依赖风扇已接线**——它只是让引脚开始输出、让定时器开始计数。
     *    真正的上电顺序是「板子先跑起来，再给风扇 12V」；反过来先给电，
     *    PWM 引脚还是浮空输入状态，风扇看到的悬空 = 内部上拉 = 全速。
     */
    ret = bsp_fan_init();
    if (ret != ERR_OK) {
        return ret;
    }

    /*
     * OLED 失败不算致命：屏没接好时，温度、按键与风扇的测试照样能做，
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
            if (bsp_key_take_event((key_id_t)i) != KEY_EVENT_PRESS) {
                continue;
            }

            /* 计数照旧累加——它是「消抖有没有多计/漏计」最直接的证据 */
            s_key_press_count[i]++;

            /* K1 加档、K2 减档 */
            app_fan_step_duty((i == (uint8_t)KEY_ID_K1) ? FAN_DUTY_UP : FAN_DUTY_DOWN);
        }

        /* ---- 串口：一次整块取走，收发时序与主循环解耦 ---- */
        const uint32_t got = s_uart->read(s_uart->ctx, rx_chunk, (uint32_t)sizeof(rx_chunk));
        if (got > 0U) {
            /* 先原样回显：这一条是「RX 链路通不通」最直接的证据 */
            (void)app_write_text((const char *)rx_chunk, got);

            /* 再把里面的单字符命令执行掉（继电器、蜂鸣器的切换走这里） */
            app_handle_commands(rx_chunk, got);
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
