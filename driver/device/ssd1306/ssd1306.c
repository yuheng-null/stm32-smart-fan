/**
 * @file    ssd1306.c
 * @brief   SSD1306 128x64 单色 OLED 设备驱动实现。
 *
 * 分层约定：本文件只通过 i2c_if_t 访问总线，不出现任何 HAL 符号，
 * 也不使用全局变量——所有状态都挂在 ssd1306_t 实例上。
 *
 * 显存组织（与 SSD1306 硬件一致）：
 *   屏幕纵向分 8 页，每页 8 行像素。显存里 1 个字节代表某一页上、
 *   同一列的 8 个纵向像素，bit0 是最上面那一行。
 *   因此坐标为 (x, y) 的像素落在 buffer[x + (y/8)*128] 的第 (y%8) 位。
 */

#include <stddef.h>

#include "ssd1306.h"

#include "ssd1306_reg.h"

/* ------------------------------------------------------------ 内部常量 */

/** 字模覆盖的可显示 ASCII 区间（与上游字库一致：空格到 '~'） */
#define SSD1306_FONT_FIRST_CHAR     (32U)
#define SSD1306_FONT_LAST_CHAR      (126U)

/** 字模每行是 1 个 uint16，有效位在最高位一侧 */
#define SSD1306_FONT_MSB_MASK       (0x8000U)

/** 每页像素行数 */
#define SSD1306_PIXELS_PER_PAGE     (8U)

/** 一次 I2C 数据帧的最大长度：控制字节 + 一整页数据 */
#define SSD1306_DATA_FRAME_MAX      (SSD1306_WIDTH + 1U)

/** 上电后屏幕内部复位所需时间（数据手册要求 > 100 ms） */
#define SSD1306_POWER_ON_DELAY_MS   (100U)

/* -------------------------------------------------------------- 内部函数 */

static void ssd1306_delay(ssd1306_t *dev, uint32_t ms)
{
    if (dev->delay_ms != NULL) {
        dev->delay_ms(ms);
    }
}

/**
 * @brief 发送一条命令。
 *
 * SSD1306 的 I2C 帧：[从机地址+W] [控制字节=0x00] [命令] ...
 */
static error_t ssd1306_write_cmd(ssd1306_t *dev, uint8_t cmd)
{
    const uint8_t frame[2] = { SSD1306_CTRL_BYTE_COMMAND, cmd };

    return dev->bus->write(dev->bus->ctx, dev->addr, frame, (uint32_t)sizeof(frame));
}

/**
 * @brief 发送「命令 + 参数」，两个字节在同一个 I2C 帧里。
 */
static error_t ssd1306_write_cmd_arg(ssd1306_t *dev, uint8_t cmd, uint8_t arg)
{
    const uint8_t frame[3] = { SSD1306_CTRL_BYTE_COMMAND, cmd, arg };

    return dev->bus->write(dev->bus->ctx, dev->addr, frame, (uint32_t)sizeof(frame));
}

/**
 * @brief 发送显存数据。
 *
 * 需要在数据前拼一个控制字节 0x40。为不改动调用者的缓冲，这里拷贝一份，
 * 但 I2C 一帧内控制字节必须和数据连续，不能用两次 write 拼。
 */
static error_t ssd1306_write_data(ssd1306_t *dev, const uint8_t *data, uint32_t len)
{
    uint8_t frame[SSD1306_DATA_FRAME_MAX];

    if (len > (uint32_t)(SSD1306_DATA_FRAME_MAX - 1U)) {
        return ERR_INVALID_PARAM;
    }

    frame[0] = SSD1306_CTRL_BYTE_DATA;
    for (uint32_t i = 0U; i < len; i++) {
        frame[i + 1U] = data[i];
    }

    return dev->bus->write(dev->bus->ctx, dev->addr, frame, len + 1U);
}

/* ---------------------------------------------------------------- 初始化 */

error_t ssd1306_init(ssd1306_t *dev)
{
    if (dev == NULL) {
        return ERR_INVALID_PARAM;
    }
    if ((dev->bus == NULL) || (dev->bus->write == NULL)) {
        return ERR_INVALID_PARAM;
    }

    dev->initialized = false;
    dev->display_on  = false;
    dev->cursor_x    = 0U;
    dev->cursor_y    = 0U;

    /* 屏幕内部上电复位期间不接受命令 */
    ssd1306_delay(dev, SSD1306_POWER_ON_DELAY_MS);

    /* 先关显示，避免初始化过程中出现花屏 */
    if (ssd1306_write_cmd(dev, SSD1306_CMD_DISPLAY_OFF) != ERR_OK) {
        return ERR_COMMUNICATION;
    }

    /* 寻址模式：水平，适合整屏连续刷新 */
    if (ssd1306_write_cmd_arg(dev, SSD1306_CMD_SET_MEM_ADDR_MODE,
                              SSD1306_MEM_ADDR_MODE_HORIZONTAL) != ERR_OK) {
        return ERR_COMMUNICATION;
    }

    /* 列地址与起始行：从左上角开始 */
    if (ssd1306_write_cmd(dev, SSD1306_CMD_SET_LOW_COLUMN | 0x00U) != ERR_OK) {
        return ERR_COMMUNICATION;
    }
    if (ssd1306_write_cmd(dev, SSD1306_CMD_SET_HIGH_COLUMN | 0x00U) != ERR_OK) {
        return ERR_COMMUNICATION;
    }
    if (ssd1306_write_cmd(dev, SSD1306_CMD_SET_START_LINE | 0x00U) != ERR_OK) {
        return ERR_COMMUNICATION;
    }

    /* 对比度 */
    if (ssd1306_write_cmd_arg(dev, SSD1306_CMD_SET_CONTRAST, SSD1306_CONTRAST_MAX) != ERR_OK) {
        return ERR_COMMUNICATION;
    }

    /* 扫描方向：左右、上下都取「正常」方向 */
    if (ssd1306_write_cmd(dev, SSD1306_CMD_SEG_REMAP_FLIP) != ERR_OK) {
        return ERR_COMMUNICATION;
    }
    if (ssd1306_write_cmd(dev, SSD1306_CMD_COM_SCAN_DEC) != ERR_OK) {
        return ERR_COMMUNICATION;
    }

    /* 显示内容跟随显存，正常（非反色）显示 */
    if (ssd1306_write_cmd(dev, SSD1306_CMD_ENTIRE_DISPLAY_RAM) != ERR_OK) {
        return ERR_COMMUNICATION;
    }
    if (ssd1306_write_cmd(dev, SSD1306_CMD_NORMAL_DISPLAY) != ERR_OK) {
        return ERR_COMMUNICATION;
    }

    /* 面板参数 */
    if (ssd1306_write_cmd_arg(dev, SSD1306_CMD_SET_MULTIPLEX,
                              (uint8_t)(SSD1306_HEIGHT - 1U)) != ERR_OK) {
        return ERR_COMMUNICATION;
    }
    if (ssd1306_write_cmd_arg(dev, SSD1306_CMD_DISPLAY_OFFSET, 0x00U) != ERR_OK) {
        return ERR_COMMUNICATION;
    }
    if (ssd1306_write_cmd_arg(dev, SSD1306_CMD_SET_CLOCK_DIV,
                              SSD1306_CLOCK_DIV_DEFAULT) != ERR_OK) {
        return ERR_COMMUNICATION;
    }
    if (ssd1306_write_cmd_arg(dev, SSD1306_CMD_SET_PRECHARGE,
                              SSD1306_PRECHARGE_DEFAULT) != ERR_OK) {
        return ERR_COMMUNICATION;
    }
    if (ssd1306_write_cmd_arg(dev, SSD1306_CMD_SET_COM_PINS,
                              SSD1306_COM_PINS_ALTERNATIVE) != ERR_OK) {
        return ERR_COMMUNICATION;
    }
    if (ssd1306_write_cmd_arg(dev, SSD1306_CMD_SET_VCOMH, SSD1306_VCOMH_077) != ERR_OK) {
        return ERR_COMMUNICATION;
    }

    /* 这类模块没有外部 VCC，必须开内部电荷泵才有显示 */
    if (ssd1306_write_cmd_arg(dev, SSD1306_CMD_CHARGE_PUMP,
                              SSD1306_CHARGE_PUMP_ENABLE) != ERR_OK) {
        return ERR_COMMUNICATION;
    }

    /* 清屏并上屏，避免残留随机内容 */
    ssd1306_fill(dev, SSD1306_BLACK);
    if (ssd1306_update_screen(dev) != ERR_OK) {
        return ERR_COMMUNICATION;
    }

    if (ssd1306_write_cmd(dev, SSD1306_CMD_DISPLAY_ON) != ERR_OK) {
        return ERR_COMMUNICATION;
    }

    dev->display_on  = true;
    dev->initialized = true;

    return ERR_OK;
}

error_t ssd1306_set_display_on(ssd1306_t *dev, bool on)
{
    if (dev == NULL) {
        return ERR_INVALID_PARAM;
    }
    if (!dev->initialized) {
        return ERR_NOT_INITIALIZED;
    }

    const error_t err = ssd1306_write_cmd(dev, on ? SSD1306_CMD_DISPLAY_ON
                                                  : SSD1306_CMD_DISPLAY_OFF);
    if (err != ERR_OK) {
        return err;
    }

    dev->display_on = on;

    return ERR_OK;
}

error_t ssd1306_set_contrast(ssd1306_t *dev, uint8_t value)
{
    if (dev == NULL) {
        return ERR_INVALID_PARAM;
    }
    if (!dev->initialized) {
        return ERR_NOT_INITIALIZED;
    }

    return ssd1306_write_cmd_arg(dev, SSD1306_CMD_SET_CONTRAST, value);
}

/* ------------------------------------------------------------------ 显存 */

void ssd1306_fill(ssd1306_t *dev, ssd1306_color_t color)
{
    if (dev == NULL) {
        return;
    }

    const uint8_t pattern = (color == SSD1306_WHITE) ? 0xFFU : 0x00U;

    for (uint32_t i = 0U; i < SSD1306_BUFFER_SIZE; i++) {
        dev->buffer[i] = pattern;
    }
}

error_t ssd1306_update_screen(ssd1306_t *dev)
{
    if (dev == NULL) {
        return ERR_INVALID_PARAM;
    }

    for (uint8_t page = 0U; page < SSD1306_PAGE_COUNT; page++) {
        /* 指定当前页，并把列地址复位到 0 */
        if (ssd1306_write_cmd(dev, (uint8_t)(SSD1306_CMD_SET_PAGE_START | page)) != ERR_OK) {
            return ERR_COMMUNICATION;
        }
        if (ssd1306_write_cmd(dev, SSD1306_CMD_SET_LOW_COLUMN | 0x00U) != ERR_OK) {
            return ERR_COMMUNICATION;
        }
        if (ssd1306_write_cmd(dev, SSD1306_CMD_SET_HIGH_COLUMN | 0x00U) != ERR_OK) {
            return ERR_COMMUNICATION;
        }

        const uint8_t *page_data = &dev->buffer[(uint32_t)page * SSD1306_WIDTH];
        if (ssd1306_write_data(dev, page_data, SSD1306_WIDTH) != ERR_OK) {
            return ERR_COMMUNICATION;
        }
    }

    return ERR_OK;
}

void ssd1306_draw_pixel(ssd1306_t *dev, uint16_t x, uint16_t y, ssd1306_color_t color)
{
    if (dev == NULL) {
        return;
    }
    /* 越界直接忽略：画线/画矩形时不必在每个调用点做边界判断 */
    if ((x >= SSD1306_WIDTH) || (y >= SSD1306_HEIGHT)) {
        return;
    }

    const uint32_t index = (uint32_t)x + ((uint32_t)(y / SSD1306_PIXELS_PER_PAGE) * SSD1306_WIDTH);
    const uint8_t  bit   = (uint8_t)(1U << (y % SSD1306_PIXELS_PER_PAGE));

    if (color == SSD1306_WHITE) {
        dev->buffer[index] |= bit;
    } else {
        dev->buffer[index] &= (uint8_t)(~bit);
    }
}

error_t ssd1306_set_cursor(ssd1306_t *dev, uint16_t x, uint16_t y)
{
    if (dev == NULL) {
        return ERR_INVALID_PARAM;
    }
    if ((x >= SSD1306_WIDTH) || (y >= SSD1306_HEIGHT)) {
        return ERR_INVALID_PARAM;
    }

    dev->cursor_x = x;
    dev->cursor_y = y;

    return ERR_OK;
}

/* ------------------------------------------------------------------ 文本 */

char ssd1306_write_char(ssd1306_t *dev, char ch, const SSD1306_Font_t *font,
                        ssd1306_color_t color)
{
    if ((dev == NULL) || (font == NULL) || (font->data == NULL)) {
        return 0;
    }

    const uint8_t char_code = (uint8_t)ch;
    if ((char_code < SSD1306_FONT_FIRST_CHAR) || (char_code > SSD1306_FONT_LAST_CHAR)) {
        return 0;
    }

    const uint8_t index = (uint8_t)(char_code - SSD1306_FONT_FIRST_CHAR);

    /* 比例字体逐字符取宽，等宽字体直接用 font->width */
    const uint8_t char_width = (font->char_width != NULL) ? font->char_width[index]
                                                          : font->width;

    if (((uint32_t)dev->cursor_x + char_width > SSD1306_WIDTH) ||
        ((uint32_t)dev->cursor_y + font->height > SSD1306_HEIGHT)) {
        return 0; /* 本行放不下 */
    }

    for (uint8_t row = 0U; row < font->height; row++) {
        const uint16_t row_bits = font->data[((uint32_t)index * font->height) + row];

        for (uint8_t col = 0U; col < char_width; col++) {
            /* 字模高位对齐，左移 col 位后看最高位即可 */
            const bool lit = ((uint16_t)(row_bits << col) & SSD1306_FONT_MSB_MASK) != 0U;
            const ssd1306_color_t pixel = lit ? color : (ssd1306_color_t)(!color);

            ssd1306_draw_pixel(dev,
                               (uint16_t)(dev->cursor_x + col),
                               (uint16_t)(dev->cursor_y + row),
                               pixel);
        }
    }

    dev->cursor_x = (uint16_t)(dev->cursor_x + char_width);

    return ch;
}

uint16_t ssd1306_write_string(ssd1306_t *dev, const char *str, const SSD1306_Font_t *font,
                              ssd1306_color_t color)
{
    uint16_t written = 0U;

    if ((dev == NULL) || (str == NULL) || (font == NULL)) {
        return 0U;
    }

    while (*str != '\0') {
        if (ssd1306_write_char(dev, *str, font, color) == 0) {
            break; /* 放不下了，停止 */
        }
        written++;
        str++;
    }

    return written;
}

/* ------------------------------------------------------------------ 图形 */

void ssd1306_draw_line(ssd1306_t *dev, uint16_t x1, uint16_t y1,
                       uint16_t x2, uint16_t y2, ssd1306_color_t color)
{
    if (dev == NULL) {
        return;
    }

    /* Bresenham 直线算法：只用整数加减，误差项决定下一个点在哪个方向偏移 */
    int32_t x = (int32_t)x1;
    int32_t y = (int32_t)y1;

    const int32_t dx = (x2 > x1) ? ((int32_t)x2 - (int32_t)x1) : ((int32_t)x1 - (int32_t)x2);
    const int32_t dy = (y2 > y1) ? -((int32_t)y2 - (int32_t)y1) : ((int32_t)y1 - (int32_t)y2);
    const int32_t step_x = (x1 < x2) ? 1 : -1;
    const int32_t step_y = (y1 < y2) ? 1 : -1;

    int32_t err = dx + dy;

    for (;;) {
        ssd1306_draw_pixel(dev, (uint16_t)x, (uint16_t)y, color);

        if ((x == (int32_t)x2) && (y == (int32_t)y2)) {
            break;
        }

        const int32_t err2 = 2 * err;
        if (err2 >= dy) {
            err += dy;
            x += step_x;
        }
        if (err2 <= dx) {
            err += dx;
            y += step_y;
        }
    }
}

void ssd1306_draw_rectangle(ssd1306_t *dev, uint16_t x1, uint16_t y1,
                            uint16_t x2, uint16_t y2, ssd1306_color_t color)
{
    if (dev == NULL) {
        return;
    }

    ssd1306_draw_line(dev, x1, y1, x2, y1, color);
    ssd1306_draw_line(dev, x1, y2, x2, y2, color);
    ssd1306_draw_line(dev, x1, y1, x1, y2, color);
    ssd1306_draw_line(dev, x2, y1, x2, y2, color);
}

void ssd1306_fill_rectangle(ssd1306_t *dev, uint16_t x1, uint16_t y1,
                            uint16_t x2, uint16_t y2, ssd1306_color_t color)
{
    if (dev == NULL) {
        return;
    }

    for (uint16_t y = y1; y <= y2; y++) {
        for (uint16_t x = x1; x <= x2; x++) {
            ssd1306_draw_pixel(dev, x, y, color);
        }
    }
}
