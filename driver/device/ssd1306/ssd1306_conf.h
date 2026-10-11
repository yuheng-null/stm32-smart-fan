/**
 * @file    ssd1306_conf.h
 * @brief   SSD1306 驱动的编译期配置。
 *
 * 字体宏的名字沿用了上游 afiskon/stm32-ssd1306 的约定：
 * 字库文件 ssd1306_fonts.c / ssd1306_fonts.h 是原样引入的，
 * 靠这些宏决定哪些字模参与编译。改名字就要连字库一起改，故保持不变。
 */

#ifndef SSD1306_CONF_H
#define SSD1306_CONF_H

/** 显示屏宽度（像素）。0.96 寸模块为 128。 */
#define SSD1306_WIDTH               (128U)

/** 显示屏高度（像素）。0.96 寸模块为 64。 */
#define SSD1306_HEIGHT              (64U)

/*
 * 启用的字体。每多开一种，Flash 里就多一份字模：
 *   6x8   ≈ 1.5 KB   小号，适合一屏多行
 *   7x10  ≈ 2.3 KB   中号
 *   11x18 ≈ 6.3 KB   大号，适合显示角度/转速这类主数值
 * 目前只开这三种，够用且省 Flash；需要更大字号时再打开下面的 16x26。
 */
#define SSD1306_INCLUDE_FONT_6x8
#define SSD1306_INCLUDE_FONT_7x10
#define SSD1306_INCLUDE_FONT_11x18
/* #define SSD1306_INCLUDE_FONT_16x26 */

#endif /* SSD1306_CONF_H */
