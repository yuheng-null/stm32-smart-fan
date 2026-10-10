/**
 * @file bsp_system.h
 * @brief 板级系统服务：时钟频率与毫秒时基。
 *
 * app/ 不许直接调 HAL，所以「查主频」「取毫秒计数」这类系统服务由 bsp/ 包一层。
 * 这一层允许出现 HAL，但只做转发，不含任何业务判断。
 */
#ifndef BSP_SYSTEM_H
#define BSP_SYSTEM_H

#include <stdint.h>

/**
 * @brief 取当前 SYSCLK 频率。
 *
 * 注意这是**按 RCC 寄存器 + 编译期 HSE_VALUE 宏算出来的期望值**，
 * 不是实测值。它无法证明晶振真的是 8 MHz——能证明这件事的是串口：
 * 若实际晶振与 HSE_VALUE 不符，波特率会整体偏移，CH340 上收到的是乱码。
 *
 * @return uint32_t 单位 Hz。
 */
uint32_t bsp_system_get_sysclk_hz(void);

/**
 * @brief 取系统启动以来的毫秒计数（由 HAL SysTick 维护）。
 *
 * @return uint32_t 单位毫秒，约 49.7 天回绕。判断间隔请用无符号减法，
 *                  不要用 `a > b` 直接比较。
 */
uint32_t bsp_system_get_tick_ms(void);

#endif /* BSP_SYSTEM_H */
