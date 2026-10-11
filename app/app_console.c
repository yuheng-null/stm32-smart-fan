/**
 * @file app_console.c
 * @brief 串口控制台的实现。
 *
 * ── 为什么要有 `@ACK,<verb>,OK` 这种回执 ─────────────────────────────
 *   命令「发出去了」和「生效了」是两件事。回执让上位机（或人）能从串口流里
 *   明确看到某条命令被接受还是被拒绝，配合 `@TEL` 帧就能写出自动化回归：
 *   发命令 → 断言 @ACK → 断言随后的 @TEL 帧里字段到位。
 *
 * ── 为什么不回显 ─────────────────────────────────────────────────────
 *   遥测帧本身就在持续往外打，再叠一层字符回显只会让流更难读。
 *   命令的确认由 @ACK 负责。
 * ────────────────────────────────────────────────────────────────────
 */

#include "app/app_console.h"

#include "app/app_ir.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app/app_control.h"

#include "bsp/bsp_system.h"
#include "bsp/bsp_uart.h"

#include "interface/uart_if.h"

#include "module/console/console.h"
#include "module/telemetry/telemetry.h"

/* --------------------------------------------------------------- 配置 */

/** 单次发送的阻塞上限。115200 下发几十字节只需几百微秒。 */
#define APP_CONSOLE_TX_TIMEOUT_MS   (200U)

/** 一次从接收缓冲取走的字节数上限。 */
#define APP_CONSOLE_RX_CHUNK        (64U)

/** 拼一行回执用的缓冲。 */
#define APP_CONSOLE_LINE_SIZE       (64U)

/* --------------------------------------------------------------- 状态 */

static const uart_if_t  *s_uart = NULL;
static console_parser_t  s_parser;
static telemetry_format_t s_format = TELEMETRY_FMT_HUMAN;
static uint16_t          s_seq     = 0U;
static bool              s_ready   = false;

/* ----------------------------------------------------------------- 内部 */

/** @brief 把 console 的模式编码翻译成状态机的命令。 */
static fan_cmd_t console_mode_to_cmd(int32_t console_mode)
{
    switch (console_mode) {
    case 0:  return FAN_CMD_MODE_OFF;
    case 1:  return FAN_CMD_MODE_AUTO;
    case 2:  return FAN_CMD_MODE_MANUAL;
    default: return FAN_CMD_NONE;
    }
}

/** @brief 发一行回执。 */
static void console_reply(const char *verb, error_t result)
{
    char line[APP_CONSOLE_LINE_SIZE];
    int  n;

    if (result == ERR_OK) {
        n = snprintf(line, sizeof(line), "@ACK,%s,OK\r\n", verb);
    } else {
        n = snprintf(line, sizeof(line), "@ACK,%s,ERR,%d\r\n", verb, (int)result);
    }

    if (n > 0) {
        (void)app_console_send(line, (uint32_t)n);
    }
}

/** @brief 组一帧当前状态的遥测快照。 */
static bool console_make_sample(telemetry_sample_t *sample)
{
    app_status_t status;

    if (!app_control_get_status(&status)) {
        return false;
    }

    sample->seq          = s_seq;
    sample->tick_ms      = bsp_system_get_tick_ms();
    sample->mode         = status.mode;
    sample->duty_percent = status.duty_percent;
    sample->temp_milli_c = status.temp_milli_c;
    sample->temp_valid   = status.temp_valid;
    sample->adc_raw      = status.adc_raw;
    sample->rpm          = status.rpm;
    sample->target_rpm   = status.target_rpm;
    sample->alarms       = status.alarms;

    return true;
}

/**
 * @brief IR 命令：看遥控接收状态，或者开关学习模式。
 *
 * 学习模式只回显收到的码、不动风扇——换遥控器时正对着风扇按键，不会误触发。
 */
static void console_handle_ir(const console_command_t *cmd)
{
    if ((cmd->arg_i[0] == CONSOLE_IR_LEARN_ON) || (cmd->arg_i[0] == CONSOLE_IR_LEARN_OFF)) {
        console_reply(cmd->verb,
                      app_ir_set_learn(cmd->arg_i[0] == CONSOLE_IR_LEARN_ON));
        return;
    }

    app_ir_status_t ir;

    if (!app_ir_get_status(&ir)) {
        console_reply(cmd->verb, ERR_NOT_INITIALIZED);
        return;
    }

    char line[APP_CONSOLE_LINE_SIZE * 2U];
    int  n;

    if (ir.have_last) {
        n = snprintf(line, sizeof(line),
                     "  ir: last addr=0x%04X cmd=0x%02X (%s NEC)\r\n",
                     (unsigned)ir.address, (unsigned)ir.command,
                     ir.extended ? "扩展" : "标准");
    } else {
        n = snprintf(line, sizeof(line), "  ir: 还没收到过一帧\r\n");
    }

    if (n > 0) {
        (void)app_console_send(line, (uint32_t)n);
    }

    /*
     * 这一行同时回答了「遥控灵不灵」的两个问题：
     *   · frames 一直不涨 —— 压根没收到，查接收头接线与供电；
     *   · rejects 涨而 frames 不涨 —— 收得到但解不出，看 last 是卡在哪一段
     *     （引导码? 位? 反码校验?），分别指向接线干扰、遥控器不兼容、
     *     以及「码是对的但校验不过」这三类不同的问题；
     *   · dropped 非 0 —— 主循环被长时间阻塞，边沿在缓冲里溢出了。
     */
    n = snprintf(line, sizeof(line),
                 "  ir: frames=%lu repeats=%lu rejects=%lu (last: %s)"
                 " | edges=%lu dropped=%lu%s\r\n",
                 (unsigned long)ir.frames, (unsigned long)ir.repeats,
                 (unsigned long)ir.rejects, ir.last_reject,
                 (unsigned long)ir.edges, (unsigned long)ir.dropped,
                 ir.learn ? "  [学习模式]" : "");

    if (n > 0) {
        (void)app_console_send(line, (uint32_t)n);
    }

    console_reply(cmd->verb, ERR_OK);
}

/** @brief 执行一条解析出来的命令。 */
static void console_execute(const console_command_t *cmd, error_t parse_result)
{
    if (parse_result != ERR_OK) {
        console_reply(cmd->verb, parse_result);
        return;
    }

    switch (cmd->id) {
    case CONSOLE_CMD_HELP:
        /* 帮助本身就是给眼睛看的，不需要回执。 */
        (void)app_console_send(console_help_text(), (uint32_t)strlen(console_help_text()));
        return;

    case CONSOLE_CMD_STATUS: {
        telemetry_sample_t sample;

        if (console_make_sample(&sample)) {
            char   line[APP_CONSOLE_LINE_SIZE * 2U];
            size_t len = 0U;

            if (telemetry_format(&sample, TELEMETRY_FMT_HUMAN,
                                 line, sizeof(line), &len) == ERR_OK) {
                (void)app_console_send(line, (uint32_t)len);
            }
        }

        /*
         * 串口链路诊断与设定值。
         *
         * 它们刻意**不放进 `@TEL` 帧**——帧的字段顺序是对外契约，加字段会
         * 让所有上位机脚本一起失效。而 drop/err 是排查串口问题最直接的指标
         * （接线松、没共地、波特率不匹配时会持续上涨），不能因为「不好塞进帧」
         * 就丢掉，所以放在 STATUS 里按需打印。
         */
        app_status_t status;

        if (app_control_get_status(&status)) {
            char line[APP_CONSOLE_LINE_SIZE * 2U];

            const int n = snprintf(line, sizeof(line),
                                   "  link: rx_drop=%lu rx_err=%lu"
                                   " | Tset=%ld mC  manual=%u%%\r\n",
                                   (unsigned long)bsp_uart_get_rx_drop_count(),
                                   (unsigned long)bsp_uart_get_rx_error_count(),
                                   (long)app_control_get_target_temp(),
                                   (unsigned)status.manual_duty_percent);

            if (n > 0) {
                (void)app_console_send(line, (uint32_t)n);
            }
        }

        console_reply(cmd->verb, ERR_OK);
        return;
    }

    case CONSOLE_CMD_MODE:
        console_reply(cmd->verb,
                      app_control_dispatch_cmd(console_mode_to_cmd(cmd->arg_i[0]), 0));
        return;

    case CONSOLE_CMD_MANUAL:
        console_reply(cmd->verb,
                      app_control_dispatch_cmd(FAN_CMD_MANUAL_SET, cmd->arg_i[0]));
        return;

    case CONSOLE_CMD_TEMP:
        console_reply(cmd->verb, app_control_set_target_temp(cmd->arg_i[0]));
        return;

    case CONSOLE_CMD_PID:
        console_reply(cmd->verb,
                      app_control_set_gains(cmd->arg_f[0], cmd->arg_f[1]));
        return;

    case CONSOLE_CMD_STREAM:
        s_format = (cmd->arg_i[0] == 1) ? TELEMETRY_FMT_CSV : TELEMETRY_FMT_HUMAN;

        /* 切到 CSV 时先发一行表头，上位机可以直接喂给绘图脚本。 */
        if (s_format == TELEMETRY_FMT_CSV) {
            const char *header = telemetry_csv_header();
            (void)app_console_send(header, (uint32_t)strlen(header));
        }
        console_reply(cmd->verb, ERR_OK);
        return;

    case CONSOLE_CMD_RESET:
        console_reply(cmd->verb, app_control_dispatch_cmd(FAN_CMD_RESET, 0));
        return;

    case CONSOLE_CMD_IR:
        console_handle_ir(cmd);
        return;

    case CONSOLE_CMD_UNKNOWN:
    default:
        console_reply(cmd->verb, ERR_INVALID_PARAM);
        return;
    }
}

/* ----------------------------------------------------------------- 对外 */

error_t app_console_init(void)
{
    s_uart = bsp_uart_get();

    if (s_uart == NULL) {
        return ERR_NOT_INITIALIZED;
    }

    console_parser_reset(&s_parser);

    s_format = TELEMETRY_FMT_HUMAN;
    s_seq    = 0U;
    s_ready  = true;

    return ERR_OK;
}

void app_console_poll(void)
{
    if (!s_ready) {
        return;
    }

    uint8_t chunk[APP_CONSOLE_RX_CHUNK];

    const uint32_t got = s_uart->read(s_uart->ctx, chunk, (uint32_t)sizeof(chunk));

    for (uint32_t i = 0U; i < got; i++) {
        console_command_t cmd;

        const error_t result = console_parser_feed(&s_parser, chunk[i], &cmd);

        if (result == ERR_NOT_READY) {
            continue;       /* 行还没结束，继续攒 */
        }

        console_execute(&cmd, result);
    }
}

void app_console_task(void)
{
    if (!s_ready) {
        return;
    }

    telemetry_sample_t sample;

    if (!console_make_sample(&sample)) {
        return;
    }

    char   line[APP_CONSOLE_LINE_SIZE * 2U];
    size_t len = 0U;

    if (telemetry_format(&sample, s_format, line, sizeof(line), &len) == ERR_OK) {
        (void)app_console_send(line, (uint32_t)len);
    }

    s_seq++;
}

error_t app_console_send(const char *text, uint32_t len)
{
    if ((s_uart == NULL) || (text == NULL) || (len == 0U)) {
        return ERR_INVALID_PARAM;
    }

    return s_uart->write(s_uart->ctx, (const uint8_t *)text, len,
                         APP_CONSOLE_TX_TIMEOUT_MS);
}
