/**
 * @file bsp_oled.h
 * @brief 板级 OLED（SSD1306 128x64）支持：把设备驱动绑到本板的 I2C 总线上。
 *
 * ── 接线 ────────────────────────────────────────────────────────────
 *   模块丝印        接到
 *   -----------     ----------------
 *   GND        ──   GND
 *   VCC        ──   3V3        （不要接 5V：SSD1306 逻辑与电荷泵都按 3.3V 设计）
 *   SCL        ──   PB6
 *   SDA        ──   PB7
 *
 *   四针模块的丝印顺序各厂不一，接线前按丝印核对，别按位置猜。
 *
 *   上拉：模块板载 4.7 kΩ 已在 SCL/SDA 上，无需外接。
 * ────────────────────────────────────────────────────────────────────
 *
 * 本模块只负责「这块板上，屏挂在哪条总线、地址是多少、字怎么画上去」；
 * SSD1306 寄存器怎么配、显存怎么组织，全部在 driver/device/ssd1306/ 里，
 * 那份代码不含任何 STM32 符号，换芯片不用改。
 *
 * ── 为什么把绘制也收在这里 ──────────────────────────────────────────
 *   本头文件**不导出 ssd1306_t**，绘制走下面这组与器件无关的接口。
 *   这样 app/ 那一层（app_display.c 负责排版）就不必认识 SSD1306：它只调用
 *   「在 (x,y) 画一串字」和「上屏」。docs/architecture.md 的依赖表里
 *   APP 只许调 MODULE / BSP / INTERFACE，而「不许放外设驱动」正是 APP 层的
 *   硬约束——把驱动调用关在 BSP 里面，那条约束才是真的。
 *   代价是 BSP 多了一层很薄的转发；换来的是换屏幕时 app_display.c 一行不动。
 * ────────────────────────────────────────────────────────────────────
 */
#ifndef BSP_OLED_H
#define BSP_OLED_H

#include <stdint.h>

#include "interface/error.h"

/** 屏幕像素宽度。与 driver/device/ssd1306/ssd1306_conf.h 的 SSD1306_WIDTH 同源，
 *  改了一边没改另一边会在 bsp_oled.c 里编译不过（那里有静态断言）。 */
#define BSP_OLED_WIDTH_PX       (128U)

/** 字号。字符宽度是布局要用的量，由 bsp_oled_text_width() 给出，不要自己乘。 */
typedef enum {
    BSP_OLED_FONT_SMALL = 0,    /**< 6×8：高 8 像素，一行可放 21 个字符 */
    BSP_OLED_FONT_LARGE,        /**< 11×18：大字号，高 18 像素 */
} bsp_oled_font_t;

/**
 * @brief 初始化 OLED：接入 I2C 总线、探测地址、配置屏幕。
 *
 * 地址按 0x3C → 0x3D 的顺序探测（0.96 寸模块两者都有，取决于模块上
 * SA0 的接法），谁应答就用谁；都不应答则返回 ERR_COMMUNICATION。
 *
 * 设备实例由本模块自己持有（本板只有一块屏），调用方不需要、也拿不到它。
 *
 * @return error_t ERR_OK 屏幕已点亮；
 *                 ERR_NOT_INITIALIZED 总线尚未初始化；ERR_COMMUNICATION 屏幕无应答。
 *
 * @note 必须先调用 bsp_i2c_init()。
 */
error_t bsp_oled_init(void);

/**
 * @brief 返回初始化时实际用上的从机地址。
 *
 * @return uint8_t 7 位地址；未初始化时为 0。
 */
uint8_t bsp_oled_get_addr(void);

/**
 * @brief 把本地显存清成黑色。
 *
 * @note 只动本地显存，要看到效果还得 bsp_oled_flush()。
 */
void bsp_oled_clear(void);

/**
 * @brief 一串文本画到本地显存上。
 *
 * @param x    左上角横坐标（像素）。
 * @param y    左上角纵坐标（像素）。
 * @param text 以 NUL 结尾的字符串。
 * @param font 字号。
 *
 * @note 超出屏幕范围的部分**不报错**，直接看不见——所以排版时必须自己保证
 *       画得下（OLED 每行能放几个字符，由 display_view 那一层的宽度约束负责）。
 *       仅写本地显存，上屏要靠 bsp_oled_flush()。
 */
void bsp_oled_draw_text(uint16_t x, uint16_t y, const char *text, bsp_oled_font_t font);

/**
 * @brief 这串文本按给定字号画出来有多宽（像素）。排版时用它算右对齐/跟随位置。
 *
 * @return uint16_t 宽度（像素）；text 为空指针时返回 0。
 */
uint16_t bsp_oled_text_width(const char *text, bsp_oled_font_t font);

/**
 * @brief 把本地显存整块推给屏幕。
 *
 * ⚠️ **这是主循环里最耗时的一步**：软件 I2C 位翻转搬 1 KB，几十毫秒量级。
 *    调用方（app_display）据此做了「内容没变就不刷」的优化。
 */
void bsp_oled_flush(void);

/**
 * @brief 扫描总线，列出所有应答的 7 位地址。
 *
 * 调试用：屏不亮时先跑它，能立刻把「接线/上拉有问题（一个都没有）」
 * 和「地址不对（有别的地址）」分开。
 *
 * @param found 结果数组，按升序填入应答的地址。
 * @param max   数组容量。
 *
 * @return uint8_t 总线上一共有多少个器件（可能大于 max，此时只填了前 max 个）。
 */
uint8_t bsp_oled_bus_scan(uint8_t *found, uint8_t max);

#endif /* BSP_OLED_H */
