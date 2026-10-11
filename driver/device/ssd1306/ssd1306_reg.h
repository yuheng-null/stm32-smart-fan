/**
 * @file    ssd1306_reg.h
 * @brief   SSD1306 的命令字定义（数据手册 Section 9 "Command Table"）。
 *
 * 所有命令集中在这里，驱动代码里不允许出现裸的 0xA8、0xD5 这类魔数——
 * 否则过两周再回来看，没人知道 0xD9 后面的 0x22 是什么意思。
 */

#ifndef DRIVER_DEVICE_SSD1306_REG_H
#define DRIVER_DEVICE_SSD1306_REG_H

/* ---- 基础控制 ---- */
#define SSD1306_CMD_DISPLAY_OFF             (0xAEU)  /**< 关闭显示（进入睡眠） */
#define SSD1306_CMD_DISPLAY_ON              (0xAFU)  /**< 开启显示 */

/* ---- 地址模式与寻址 ---- */
#define SSD1306_CMD_SET_MEM_ADDR_MODE       (0x20U)  /**< 设置内存寻址模式，后跟 1 字节 */
#define SSD1306_MEM_ADDR_MODE_HORIZONTAL    (0x00U)  /**< 水平寻址（整屏连续写） */
#define SSD1306_MEM_ADDR_MODE_VERTICAL      (0x01U)  /**< 垂直寻址 */
#define SSD1306_MEM_ADDR_MODE_PAGE          (0x02U)  /**< 页寻址（上电默认） */
#define SSD1306_CMD_SET_COL_ADDR            (0x21U)  /**< 设置列地址范围，后跟 2 字节 */
#define SSD1306_CMD_SET_PAGE_ADDR           (0x22U)  /**< 设置页地址范围，后跟 2 字节 */
#define SSD1306_CMD_SET_LOW_COLUMN          (0x00U)  /**< 列地址低 4 位（0x00 | n） */
#define SSD1306_CMD_SET_HIGH_COLUMN         (0x10U)  /**< 列地址高 4 位（0x10 | n） */
#define SSD1306_CMD_SET_START_LINE          (0x40U)  /**< 显示起始行（0x40 | n） */
#define SSD1306_CMD_SET_PAGE_START          (0xB0U)  /**< 页起始地址（0xB0 | page），页寻址用 */

/* ---- 对比度 ---- */
#define SSD1306_CMD_SET_CONTRAST            (0x81U)  /**< 对比度，后跟 1 字节，复位值 0x7F */

/* ---- 扫描方向 ---- */
#define SSD1306_CMD_SEG_REMAP_NORMAL        (0xA0U)  /**< 列地址 0 映射到 SEG0 */
#define SSD1306_CMD_SEG_REMAP_FLIP          (0xA1U)  /**< 左右镜像 */
#define SSD1306_CMD_COM_SCAN_INC            (0xC0U)  /**< 行扫描正向（上下镜像） */
#define SSD1306_CMD_COM_SCAN_DEC            (0xC8U)  /**< 行扫描反向（正常方向） */

/* ---- 显示内容控制 ---- */
#define SSD1306_CMD_ENTIRE_DISPLAY_RAM      (0xA4U)  /**< 按 RAM 内容显示 */
#define SSD1306_CMD_ENTIRE_DISPLAY_ON       (0xA5U)  /**< 无视 RAM，整屏点亮 */
#define SSD1306_CMD_NORMAL_DISPLAY          (0xA6U)  /**< 正常显示 */
#define SSD1306_CMD_INVERSE_DISPLAY         (0xA7U)  /**< 反色显示 */

/* ---- 面板参数 ---- */
#define SSD1306_CMD_SET_MULTIPLEX           (0xA8U)  /**< 多路复用比 = 行数 - 1，后跟 1 字节 */
#define SSD1306_CMD_DISPLAY_OFFSET          (0xD3U)  /**< 显示垂直偏移，后跟 1 字节 */
#define SSD1306_CMD_SET_CLOCK_DIV           (0xD5U)  /**< 时钟分频/振荡频率，后跟 1 字节 */
#define SSD1306_CMD_SET_PRECHARGE           (0xD9U)  /**< 预充电周期，后跟 1 字节 */
#define SSD1306_CMD_SET_COM_PINS            (0xDAU)  /**< COM 引脚硬件配置，后跟 1 字节 */
#define SSD1306_CMD_SET_VCOMH               (0xDBU)  /**< VCOMH 取消选择电平，后跟 1 字节 */

/* ---- 电荷泵 ---- */
#define SSD1306_CMD_CHARGE_PUMP             (0x8DU)  /**< 电荷泵设置，后跟 1 字节 */
#define SSD1306_CHARGE_PUMP_ENABLE          (0x14U)  /**< 内部 DC-DC 开启（模块无外部 VCC 时必须） */
#define SSD1306_CHARGE_PUMP_DISABLE         (0x10U)  /**< 内部 DC-DC 关闭 */

/* ---- 面板参数取值 ---- */
#define SSD1306_MULTIPLEX_64                (0x3FU)  /**< 64 行 - 1 */
#define SSD1306_COM_PINS_ALTERNATIVE        (0x12U)  /**< 64 行，交替行配置 */
#define SSD1306_CLOCK_DIV_DEFAULT           (0xF0U)  /**< 分频比 1，振荡频率取最高档 */
#define SSD1306_PRECHARGE_DEFAULT           (0x22U)  /**< 预充电周期，这两个值沿用了上游驱动在同类模块上的实测配置 */
#define SSD1306_VCOMH_077                   (0x20U)  /**< VCOMH = 0.77 × Vcc */
#define SSD1306_CONTRAST_DEFAULT            (0x7FU)  /**< 上电默认对比度 */
#define SSD1306_CONTRAST_MAX                (0xFFU)

/* ---- I2C 控制字节 ---- */
/*
 * SSD1306 的 I2C 帧结构：[从机地址+W] [控制字节] [数据...]
 * 控制字节的 bit6 (D/C#) 决定后面跟的是命令还是显存数据。
 */
#define SSD1306_CTRL_BYTE_COMMAND           (0x00U)  /**< 后续字节是命令 */
#define SSD1306_CTRL_BYTE_DATA              (0x40U)  /**< 后续字节是显存数据 */

#endif /* DRIVER_DEVICE_SSD1306_REG_H */
