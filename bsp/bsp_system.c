/**
 * @file bsp_system.c
 * @brief 板级系统服务的实现（STM32F407 + CubeMX 生成的 HAL）。
 */

#include "bsp/bsp_system.h"

#include "main.h"

uint32_t bsp_system_get_sysclk_hz(void)
{
    return HAL_RCC_GetSysClockFreq();
}

uint32_t bsp_system_get_tick_ms(void)
{
    return HAL_GetTick();
}
