/**
 * @file app_main.c
 * @brief 应用层入口：串口链路冒烟验证。
 *
 * 本层严禁出现 HAL 调用（见 docs/architecture.md 的依赖规则），
 * 所有硬件动作都经 bsp/ 暴露的 interface 契约完成。
 */

#include "app/app_main.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "bsp/bsp_system.h"
#include "bsp/bsp_uart.h"
#include "interface/error.h"
#include "interface/uart_if.h"

/** 心跳周期。 */
#define APP_HEARTBEAT_PERIOD_MS (1000U)

/** 一次从接收缓冲取走的字节数上限。 */
#define APP_RX_CHUNK_SIZE (64U)

/** 单次发送的阻塞上限。115200 下发几十字节只需几百微秒，200 ms 很宽松。 */
#define APP_TX_TIMEOUT_MS (200U)

/** 动态行缓冲。心跳行最长约 90 字节，留足余量。 */
#define APP_LINE_BUFFER_SIZE (128U)

/*
 * 开机横幅的静态部分。
 *
 * 刻意拆成「静态文字 + 一行动态值」而不是塞进一个缓冲区：
 * 横幅里有中文，UTF-8 下一个汉字 3 字节，整条横幅约 330 字节。
 * 若用固定缓冲拼接，一旦缓冲不够，snprintf 会静默截断
 * （gcc 的 -Wformat-truncation 会警告，但运行时本身不会报错）。
 * 静态文字直接以字面量发送，既不占 RAM 也根本不会截断。
 */
static const char APP_BANNER_HEAD[] = "\r\n==== smart_fan boot ====\r\n";

static const char APP_BANNER_TAIL[] =
    "USART1 = 115200-8-N-1, PA9=TX PA10=RX\r\n"
    "上面这几行在 CH340 串口助手里能正常显示（不是乱码），\r\n"
    "就说明 8MHz 晶振 + 168MHz 时钟树 + 波特率分频 全部正确。\r\n"
    "接收走中断 + 环形缓冲：输入任意内容会被原样回显。\r\n"
    "心跳里的 drop / err 持续为 0 才算收发都健康。\r\n"
    "\r\n";

/** 调试串口契约。app_main_init() 里绑定，之后只读。 */
static const uart_if_t *s_uart;

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
 *
 * @param text 文本首地址。
 * @param len  字节数，调用方传 sizeof(数组) - 1U。
 *
 * @return error_t ERR_OK 已发出；ERR_INVALID_PARAM 入参非法。
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
 * @param text 已格式化的文本。
 * @param len  snprintf 的返回值；为负表示编码错误。
 * @param cap  缓冲区容量，用于识别截断。
 *
 * @return error_t ERR_OK 已发出；ERR_INVALID_PARAM 编码失败或被截断。
 */
static error_t app_send_formatted(const char *text, int len, size_t cap)
{
    if ((len <= 0) || ((size_t)len >= cap)) {
        return ERR_INVALID_PARAM;
    }
    return app_write_text(text, (uint32_t)len);
}

/**
 * @brief 发送开机横幅：静态头 + 一行动态主频 + 静态尾。
 *
 * @return error_t 三段全成功才返回 ERR_OK。
 */
static error_t app_send_banner(void)
{
    char    line[APP_LINE_BUFFER_SIZE];
    int     len;
    error_t ret;

    ret = app_send_literal(APP_BANNER_HEAD, sizeof(APP_BANNER_HEAD) - 1U);
    if (ret != ERR_OK) {
        return ret;
    }

    len = snprintf(line, sizeof(line), "SYSCLK = %lu Hz\r\n",
                   (unsigned long)bsp_system_get_sysclk_hz());
    ret = app_send_formatted(line, len, sizeof(line));
    if (ret != ERR_OK) {
        return ret;
    }

    return app_send_literal(APP_BANNER_TAIL, sizeof(APP_BANNER_TAIL) - 1U);
}

/**
 * @brief 发送一行心跳，附带收包统计。
 *
 * drop / err 是判断「接线与波特率是否真的健康」的直接指标：
 * 接线松、没共地、波特率不匹配时这两个数会持续上涨。
 *
 * @param heartbeat 心跳序号。
 * @param rx_total  累计收到的字节数。
 *
 * @return error_t 见 app_send_formatted()。
 */
static error_t app_send_heartbeat(uint32_t heartbeat, uint32_t rx_total)
{
    char line[APP_LINE_BUFFER_SIZE];
    int  len;

    len = snprintf(line, sizeof(line),
                   "[heartbeat %lu] tick = %lu ms  rx = %lu  drop = %lu  err = %lu\r\n",
                   (unsigned long)heartbeat,
                   (unsigned long)bsp_system_get_tick_ms(),
                   (unsigned long)rx_total,
                   (unsigned long)bsp_uart_get_rx_drop_count(),
                   (unsigned long)bsp_uart_get_rx_error_count());

    return app_send_formatted(line, len, sizeof(line));
}

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

    return ERR_OK;
}

void app_main_run(void)
{
    uint8_t  rx_chunk[APP_RX_CHUNK_SIZE];
    uint32_t rx_total     = 0U;
    uint32_t heartbeat    = 0U;
    uint32_t next_beat_ms = 0U;
    bool     banner_done  = false;
    uint32_t got;

    for (;;) {
        /* ---- 开机横幅：发不出去就下一轮再试 ---- */
        if (!banner_done) {
            if (app_send_banner() == ERR_OK) {
                banner_done  = true;
                next_beat_ms = bsp_system_get_tick_ms() + APP_HEARTBEAT_PERIOD_MS;
            }
            continue;
        }

        /* ---- 回显：把缓冲里攒下的数据**一次整块**取走并原样发回 ----
         *
         * 这里不是一字节一字节地搬运。中断已经把字节按线路速率存进缓冲了，
         * 主循环一次取一整块，收发时序彻底解耦——主循环去发心跳的那几毫秒
         * 不再影响接收。 */
        got = s_uart->read(s_uart->ctx, rx_chunk, (uint32_t)sizeof(rx_chunk));
        if (got > 0U) {
            rx_total += got;
            if (app_write_text((const char *)rx_chunk, got) != ERR_OK) {
                continue;
            }
        }

        /* ---- 心跳 ---- */
        /* 用无符号减法判到期：tick 回绕时仍然正确，直接比较大小则不行。 */
        if ((int32_t)(bsp_system_get_tick_ms() - next_beat_ms) >= 0) {
            heartbeat++;
            next_beat_ms += APP_HEARTBEAT_PERIOD_MS;

            if (app_send_heartbeat(heartbeat, rx_total) != ERR_OK) {
                continue;
            }
        }
    }
}
