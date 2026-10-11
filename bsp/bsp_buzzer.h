/**
 * @file bsp_buzzer.h
 * @brief 板级有源蜂鸣器模块（IO ← PB1，**低电平触发**）。
 *
 * ── 接线 ────────────────────────────────────────────────────────────
 *   模块 IO   ── PB1
 *   模块 VCC  ── 3V3（板子）
 *   模块 GND  ── GND
 * ────────────────────────────────────────────────────────────────────
 *
 * ── 「有源」意味着什么 ──────────────────────────────────────────────
 *   有源蜂鸣器内部自带振荡电路，**给电就响**，MCU 只需要给一个电平，
 *   不需要（也不能）用 PWM 去驱动音调。
 *   （无源蜂鸣器正相反：要外部给方波，频率决定音高。驱动方式完全不同，
 *     换器件时这一点必须先确认，否则会出现"给了方波却不响"。）
 *
 * ── 初始电平 ────────────────────────────────────────────────────────
 *   CubeMX 生成的 MX_GPIO_Init() 已把它置为**高**（静音）。
 *   这一点很重要：低电平触发若不显式设初值，默认的低电平会在上电到
 *   应用层接管之间让蜂鸣器「叫一嗓子」。
 *
 * ⚠️ 本模块是新增的 3.3V 负载。板子 3.3V 轨的余量此前没有实测过
 *    （有过一次 OLED 一度不亮的未定论现象），所以蜂鸣器响的同时
 *    要留意串口有没有复位、OLED 有没有闪。
 */
#ifndef BSP_BUZZER_H
#define BSP_BUZZER_H

#include <stdbool.h>

#include "interface/error.h"

/**
 * @brief 初始化（登记状态）。
 *
 * **不配置引脚**——引脚模式与初始电平（推挽 + 高）由 CubeMX 生成的
 * MX_GPIO_Init() 完成。因此必须在它**之后**调用。
 *
 * @return error_t ERR_OK 成功。
 */
error_t bsp_buzzer_init(void);

/**
 * @brief 让蜂鸣器响 / 停。
 *
 * @param on true = 响；false = 静音。
 *
 * @return error_t ERR_OK 成功；ERR_NOT_INITIALIZED 还没初始化。
 */
error_t bsp_buzzer_set(bool on);

/**
 * @brief 取蜂鸣器当前的命令状态。
 *
 * @return bool true 表示当前命令是响。
 */
bool bsp_buzzer_is_on(void);

#endif /* BSP_BUZZER_H */
