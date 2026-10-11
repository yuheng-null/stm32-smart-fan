/**
 * @file    ssd1306.h
 * @brief   SSD1306 128x64 单色 OLED 设备驱动。
 *
 * 本驱动只依赖抽象总线接口 i2c_if_t，不含任何 HAL / STM32 专有符号，
 * 因此换 MCU 平台不必改动本文件，也可以在 PC 上注入 mock 总线做单元测试。
 *
 * 字库（ssd1306_fonts.c/h）原样取自 afiskon/stm32-ssd1306（MIT 协议），
 * 因此本文件里的 SSD1306_Font_t 布局与上游严格保持一致，不得调整字段顺序。
 *
 * 用法：
 * @code
 *   static ssd1306_t oled;
 *
 *   oled.bus      = i2c_soft_iface(&bus);   // 或任何实现了 i2c_if_t 的总线
 *   oled.addr     = SSD1306_I2C_ADDR_7BIT;
 *   oled.delay_ms = bsp_oled_delay_ms;
 *   (void)ssd1306_init(&oled);
 *
 *   ssd1306_fill(&oled, SSD1306_BLACK);
 *   ssd1306_set_cursor(&oled, 0U, 0U);
 *   ssd1306_write_string(&oled, "Hello", &Font_11x18, SSD1306_WHITE);
 *   (void)ssd1306_update_screen(&oled);
 * @endcode
 */

#ifndef DRIVER_DEVICE_SSD1306_H
#define DRIVER_DEVICE_SSD1306_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "interface/error.h"
#include "interface/i2c_if.h"

#include "ssd1306_conf.h"   /* 同目录，编译器会先在包含者所在目录里找 */

/** 显存大小（字节）：128 x 64 / 8 = 1024 */
#define SSD1306_BUFFER_SIZE     (((uint32_t)SSD1306_WIDTH * SSD1306_HEIGHT) / 8U)

/** 显存页数：每页 8 行像素 */
#define SSD1306_PAGE_COUNT      (SSD1306_HEIGHT / 8U)

/** 常见的 0.96 寸模块 I2C 7 位地址（SA0 接地） */
#define SSD1306_I2C_ADDR_7BIT   (0x3CU)

/** 取反用：颜色只有「亮」和「灭」两态 */
typedef enum {
    SSD1306_BLACK = 0,      /**< 像素灭 */
    SSD1306_WHITE = 1,      /**< 像素亮 */
} ssd1306_color_t;

/**
 * @brief 字模描述。
 *
 * @warning 字段顺序与上游 afiskon/stm32-ssd1306 完全一致，字库文件
 *          ssd1306_fonts.c 里是用位置初始化列表 {宽, 高, 数据, 比例表}
 *          填充的，调整顺序会导致字模错位，改动前务必同步修改字库。
 */
typedef struct {
    const uint8_t         width;        /**< 字宽（像素） */
    const uint8_t         height;       /**< 字高（像素） */
    const uint16_t *const data;         /**< 字模数据，每行 1 个 uint16，高位对齐 */
    const uint8_t *const  char_width;   /**< 比例字体的逐字符宽度表；等宽字体为 NULL */
} SSD1306_Font_t;

/**
 * @brief 毫秒级延时回调。
 *
 * 设备驱动不允许直接调用 HAL_Delay（会把驱动绑死在某个平台上），
 * 因此由上层注入。PC 上测试时注入一个空实现即可。
 */
typedef void (*ssd1306_delay_ms_fn)(uint32_t ms);

/**
 * @brief SSD1306 设备实例。
 *
 * 所有状态都放在这里，驱动内部不使用全局变量，
 * 因此同一条或多条总线上可以并存多块屏。
 */
typedef struct {
    i2c_if_t            *bus;                       /**< 总线接口（不持有所有权） */
    uint8_t              addr;                      /**< 从机 7 位地址 */
    ssd1306_delay_ms_fn  delay_ms;                  /**< 延时回调，可为 NULL（则不延时） */
    uint8_t              buffer[SSD1306_BUFFER_SIZE]; /**< 本地显存，1 字节 = 8 个纵向像素 */
    uint16_t             cursor_x;                  /**< 文本光标 X */
    uint16_t             cursor_y;                  /**< 文本光标 Y */
    bool                 initialized;               /**< 已初始化标志 */
    bool                 display_on;                /**< 显示开关当前状态 */
} ssd1306_t;

/* ------------------------------------------------------------ 初始化类 */

/**
 * @brief 初始化屏幕。
 *
 * @param dev 设备实例。调用前必须由调用者填好 bus、addr、delay_ms 三个字段。
 *
 * @return error_t ERR_OK 成功；ERR_INVALID_PARAM 字段缺失；ERR_COMMUNICATION 屏幕无应答
 *
 * @note 上电后屏幕需要约 100 ms 才能接受命令，本函数内部会用 delay_ms 等待。
 */
error_t ssd1306_init(ssd1306_t *dev);

/**
 * @brief 开关显示（不影响显存内容）。
 */
error_t ssd1306_set_display_on(ssd1306_t *dev, bool on);

/**
 * @brief 设置对比度（亮度）。
 *
 * @param value 0x00 ~ 0xFF，越大越亮，复位值 0x7F
 */
error_t ssd1306_set_contrast(ssd1306_t *dev, uint8_t value);

/* -------------------------------------------------------------- 显存类 */

/**
 * @brief 用指定颜色填充整块显存（只改本地缓冲，需再 update_screen 才上屏）。
 */
void ssd1306_fill(ssd1306_t *dev, ssd1306_color_t color);

/**
 * @brief 把整块本地显存刷到屏幕。
 *
 * @return error_t 任一分页写失败即返回错误
 */
error_t ssd1306_update_screen(ssd1306_t *dev);

/**
 * @brief 画一个像素。坐标越界会被忽略（不报错，方便画图时不做边界判断）。
 */
void ssd1306_draw_pixel(ssd1306_t *dev, uint16_t x, uint16_t y, ssd1306_color_t color);

/**
 * @brief 设置文本光标位置。
 */
error_t ssd1306_set_cursor(ssd1306_t *dev, uint16_t x, uint16_t y);

/* -------------------------------------------------------------- 文本类 */

/**
 * @brief 在光标处写一个字符，并把光标右移该字符宽度。
 *
 * @return 写入成功返回该字符；越界或不可显示字符返回 0
 */
char ssd1306_write_char(ssd1306_t *dev, char ch, const SSD1306_Font_t *font,
                        ssd1306_color_t color);

/**
 * @brief 在光标处写一个以 '\0' 结尾的字符串。
 *
 * @return 实际写入的字符数
 */
uint16_t ssd1306_write_string(ssd1306_t *dev, const char *str, const SSD1306_Font_t *font,
                              ssd1306_color_t color);

/* -------------------------------------------------------------- 图形类 */

/** @brief 画线段（Bresenham 算法） */
void ssd1306_draw_line(ssd1306_t *dev, uint16_t x1, uint16_t y1,
                       uint16_t x2, uint16_t y2, ssd1306_color_t color);

/** @brief 画矩形边框 */
void ssd1306_draw_rectangle(ssd1306_t *dev, uint16_t x1, uint16_t y1,
                            uint16_t x2, uint16_t y2, ssd1306_color_t color);

/** @brief 画实心矩形 */
void ssd1306_fill_rectangle(ssd1306_t *dev, uint16_t x1, uint16_t y1,
                            uint16_t x2, uint16_t y2, ssd1306_color_t color);

#endif /* DRIVER_DEVICE_SSD1306_H */
