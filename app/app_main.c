/**
 * @file app_main.c
 * @brief 启动编排与时间触发调度器。
 *
 * ── 为什么用轮转调度而不是一个 while(1) 里平铺 ───────────────────────
 *   平铺的循环里，每一步的耗时都会直接吃掉下一步的余量；只要有一处变慢
 *   （比如 OLED 整屏刷新），后面的节拍就整体漂移，而且看不出来。
 *   轮转调度把「每个任务多久跑一次」写成数据，节拍与单次耗时解耦，
 *   跑慢了也只是这一次晚一点，不会永久改变周期。
 *
 * ── 调度用累加式（next += period）而不是 now + period ────────────────
 *   后者会把每次的执行耗时累积进去，周期越来越长；累加式不会。
 *   判到期用**无符号减法**，于是 tick 回绕（约 49.7 天一次）时仍然正确。
 *
 * ── 任务顺序：先关键、后耗时 ─────────────────────────────────────────
 *   控制 → 遥测 → 显示。把最耗时的 OLED 放最后，它的阻塞只会挤占空闲时间，
 *   不会挤掉同一次循环里的控制与遥测。
 * ────────────────────────────────────────────────────────────────────
 */

#include "app/app_main.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "app/app_console.h"
#include "app/app_control.h"
#include "app/app_display.h"

#include "bsp/bsp_adc.h"
#include "bsp/bsp_buzzer.h"
#include "bsp/bsp_fan.h"
#include "bsp/bsp_i2c.h"
#include "bsp/bsp_key.h"
#include "bsp/bsp_oled.h"
#include "bsp/bsp_relay.h"
#include "bsp/bsp_system.h"
#include "bsp/bsp_uart.h"

#include "interface/error.h"

/* ----------------------------------------------------------------- 配置 */

/** 控制节拍（毫秒）。OLED 阻塞等抖动由控制器的真实 dt 吸收。 */
#define APP_TASK_CONTROL_MS     (100U)

/** 遥测与显示的节拍（毫秒）。 */
#define APP_TASK_SLOW_MS        (200U)

/** 拼一行文本用的缓冲。 */
#define APP_LINE_SIZE           (256U)

/** 总线扫描最多记录多少个地址。 */
#define APP_SCAN_MAX            (8U)

/* ----------------------------------------------------------------- 类型 */

/** 一个周期性任务。 */
typedef struct {
    uint32_t period_ms;
    uint32_t next_ms;
    void   (*run)(void);
} app_task_t;

/*
 * 任务表。顺序即执行顺序：控制最先、显示最后。
 * 显示放最后是有意的——它是最耗时的一步（软件 I2C 搬 1 KB）。
 */
static app_task_t s_tasks[] = {
    { APP_TASK_CONTROL_MS, 0U, app_control_task },
    { APP_TASK_SLOW_MS,    0U, app_console_task },
    { APP_TASK_SLOW_MS,    0U, app_display_task },
};

#define APP_TASK_COUNT  (sizeof(s_tasks) / sizeof(s_tasks[0]))

/* ----------------------------------------------------------- 串口小工具 */

/** @brief 发送一段静态文字（编译期已知长度，不复制、不缓冲）。 */
static void app_send_literal(const char *text, size_t len)
{
    if (len > 0U) {
        (void)app_console_send(text, (uint32_t)len);
    }
}

/**
 * @brief 发送 snprintf 的结果。
 *
 * @param len snprintf 的返回值；为负表示编码错误，或长到放不下（截断）时不发——
 *            宁可少一行，也不要发出一行被悄悄切掉尾部的内容。
 */
static void app_send_formatted(const char *text, int len, size_t cap)
{
    if ((len > 0) && ((size_t)len < cap)) {
        (void)app_console_send(text, (uint32_t)len);
    }
}

/* ----------------------------------------------------------- 开机自检 */

/**
 * @brief 扫描 I2C 总线并报到串口。
 *
 * 这是排障的第一入口：屏不亮时它能把「接线/上拉/供电」与「器件不在」
 * 一次分开——一个地址都没有是前者，只有 0x3C/0x3D 是正常。
 */
static void app_report_bus_scan(void)
{
    uint8_t       found[APP_SCAN_MAX];
    char          line[APP_LINE_SIZE];
    const uint8_t count = bsp_oled_bus_scan(found, (uint8_t)APP_SCAN_MAX);

    int len = snprintf(line, sizeof(line), "I2C scan: %u device(s)", (unsigned)count);
    app_send_formatted(line, len, sizeof(line));

    for (uint8_t i = 0U; (i < count) && (i < (uint8_t)APP_SCAN_MAX); i++) {
        len = snprintf(line, sizeof(line), " 0x%02X", (unsigned)found[i]);
        app_send_formatted(line, len, sizeof(line));
    }

    app_send_literal("\r\n", 2U);
}

/**
 * @brief 开机横幅：静态文字 + 一行动态主频。
 *
 * 横幅里有中文，UTF-8 下一个汉字 3 字节。静态文字直接按字面量发，
 * 既不占 RAM 也根本不会被 snprintf 静默截断。
 *
 * 主频那一行本身就是独立验证：主频算错时串口波特率跟着错，整屏是乱码，
 * 你根本读不到这一行。
 */
static error_t app_send_banner(void)
{
    static const char head[] = "\r\n==== smart_fan 智能温控风扇 ====\r\n";

    static const char tail[] =
        "架构: app -> module(纯计算,可在 PC 单测) -> interface；bsp 贴硬件\r\n"
        "温控: 温度 -> 分段曲线(+迟滞) -> 目标转速 -> 内环 PI -> PWM 占空比\r\n"
        "外设: OLED(SSD1306, 软 I2C PB6/PB7) + NTC(PA0/ADC1_IN0) + 按键(PC0/PC1)\r\n"
        "      + 风扇 PWM(PA8/TIM1_CH1, 25kHz, 开漏) + 风扇测速(PA1/TIM2_CH2)\r\n"
        "      + 继电器报警灯(PB0, 高电平触发) + 有源蜂鸣器(PB1, 低电平触发)\r\n"
        "⚠️ 风扇 12V 必须在板子跑起来之后再给：PWM 悬空 = 内部上拉 = 全速。\r\n"
        "⚠️ 风扇 0% 时仍维持约 295 rpm（4 线规范 <20% 行为未定义），不是真停。\r\n"
        "⚠️ NTC 按标称参数换算（10k / B3950）；要绝对刻度就做两点校准。\r\n"
        "输入 ? 看串口命令表。K1/K2 短按调参，K1 长按切模式，K2 长按静音。\r\n"
        "\r\n";

    char    line[APP_LINE_SIZE];
    error_t err;

    err = app_console_send(head, (uint32_t)(sizeof(head) - 1U));
    if (err != ERR_OK) {
        return err;
    }

    const int len = snprintf(line, sizeof(line), "SYSCLK = %lu Hz\r\n",
                             (unsigned long)bsp_system_get_sysclk_hz());
    if ((len <= 0) || ((size_t)len >= sizeof(line))) {
        return ERR_INVALID_PARAM;
    }
    err = app_console_send(line, (uint32_t)len);
    if (err != ERR_OK) {
        return err;
    }

    return app_console_send(tail, (uint32_t)(sizeof(tail) - 1U));
}

/** @brief 报一次屏幕探测结果。 */
static void app_report_oled(void)
{
    static const char probing[] = "OLED: probing 0x3C/0x3D ...\r\n";
    static const char ok_msg[]  = "OLED: OK\r\n";
    static const char bad_msg[] = "OLED: 无应答——检查 VCC/GND/SCL/SDA 与上拉\r\n";

    app_send_literal(probing, sizeof(probing) - 1U);
    app_report_bus_scan();

    if (app_display_is_present()) {
        app_send_literal(ok_msg, sizeof(ok_msg) - 1U);
    } else {
        app_send_literal(bad_msg, sizeof(bad_msg) - 1U);
    }
}

/* ----------------------------------------------------------------- 对外 */

error_t app_main_init(void)
{
    error_t err;

    /*
     * 顺序有实际约束，不是随意排的：
     *   · 串口最先——后面所有初始化失败都要能报到串口上；
     *   · 按键/继电器/蜂鸣器的引脚由 CubeMX 的 MX_GPIO_Init() 配好，
     *     这几个 init 只是登记状态；
     *   · 风扇必须在 MX_TIM1/TIM2_Init() 之后；
     *   · 屏幕依赖 I2C 总线，必须排在 bsp_i2c_init() 之后。
     */
    err = bsp_uart_init();
    if (err != ERR_OK) {
        return err;
    }

    err = app_console_init();
    if (err != ERR_OK) {
        return err;
    }

    err = bsp_adc_init();
    if (err != ERR_OK) {
        return err;
    }

    err = bsp_key_init();
    if (err != ERR_OK) {
        return err;
    }

    err = bsp_relay_init();
    if (err != ERR_OK) {
        return err;
    }

    err = bsp_buzzer_init();
    if (err != ERR_OK) {
        return err;
    }

    err = bsp_fan_init();
    if (err != ERR_OK) {
        return err;
    }

    err = bsp_i2c_init();
    if (err != ERR_OK) {
        return err;
    }

    /*
     * 屏幕失败不算致命：屏不亮时温控、遥测、按键全都照常，
     * 故障经串口报出来即可，不在这里中止。
     */
    (void)app_display_init();

    err = app_control_init(app_display_is_present());
    if (err != ERR_OK) {
        return err;
    }

    return ERR_OK;
}

void app_main_run(void)
{
    bool banner_done = false;

    for (;;) {
        /*
         * ---- 开机横幅 ----
         *
         * 只发一次，**发不出去也不挡后面的任务**。
         *
         * 横幅是「锦上添花」，**发不出去也不挡后面的任务**。
         *
         * 串口 TX 侧一出问题（TX 被拉死、发送超时），若把主循环停在横幅这一步，
         * 温控、按键、报警灯、蜂鸣器就**一个都不工作**，风扇停在 0%。
         * 对一个主题就是「失效保护」的系统来说，这是最不该有的耦合：
         * **诊断链路坏掉不该让冷却控制停摆**。
         *
         * 任务表照常从下一次循环开始跑（next_ms 初值为 0，第一拍立即到期）。
         * 基准时刻取当下，避免任务在上电瞬间挤成一堆。
         */
        if (!banner_done) {
            (void)app_send_banner();
            app_report_oled();

            const uint32_t now = bsp_system_get_tick_ms();

            for (size_t i = 0U; i < APP_TASK_COUNT; i++) {
                s_tasks[i].next_ms = now + s_tasks[i].period_ms;
            }

            banner_done = true;
        }

        const uint32_t now = bsp_system_get_tick_ms();

        /* 串口接收每圈都查：它不需要精确节拍，但必须跟得上。
           放在任务之前，保证命令尽早被解析。 */
        app_console_poll();

        for (size_t i = 0U; i < APP_TASK_COUNT; i++) {
            /* 无符号减法判到期：tick 回绕时依然正确，直接比大小则不行。 */
            if ((int32_t)(now - s_tasks[i].next_ms) >= 0) {
                s_tasks[i].next_ms += s_tasks[i].period_ms;
                s_tasks[i].run();
            }
        }
    }
}
