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
 * 本模块只负责「这块板上，屏挂在哪条总线、地址是多少」；
 * SSD1306 寄存器怎么配、显存怎么画，全部在 driver/device/ssd1306/ 里，
 * 那份代码不含任何 STM32 符号，换芯片不用改。
 */
#ifndef BSP_OLED_H
#define BSP_OLED_H

#include <stdint.h>

#include "interface/error.h"

#include "driver/device/ssd1306/ssd1306.h"

/**
 * @brief 初始化 OLED：接入 I2C 总线、探测地址、配置屏幕。
 *
 * 地址按 0x3C → 0x3D 的顺序探测（0.96 寸模块两者都有，取决于模块上
 * SA0 的接法），谁应答就用谁；都不应答则返回 ERR_COMMUNICATION。
 *
 * @param dev 由调用者分配的设备实例（通常静态存储期），本函数填好其字段。
 *
 * @return error_t ERR_OK 屏幕已点亮；ERR_INVALID_PARAM 入参为空；
 *                 ERR_NOT_INITIALIZED 总线尚未初始化；ERR_COMMUNICATION 屏幕无应答。
 *
 * @note 必须先调用 bsp_i2c_init()。
 */
error_t bsp_oled_init(ssd1306_t *dev);

/**
 * @brief 返回初始化时实际用上的从机地址。
 *
 * @return uint8_t 7 位地址；未初始化时为 0。
 */
uint8_t bsp_oled_get_addr(void);

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
