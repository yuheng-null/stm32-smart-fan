/**
 * @file bsp_buzzer.c
 * @brief 板级蜂鸣器实现（PB1，低电平触发）。
 */

#include "bsp/bsp_buzzer.h"

#include <stdbool.h>

#include "main.h"                   /* 借它引入 HAL 头与 BUZZER_IO_Pin / _GPIO_Port */

/*
 * 引脚的配置（推挽输出、初始高 = 静音）在 CubeMX 里，不在本文件——
 * 见 .ioc 的 PB1.GPIO_Label=BUZZER_IO 与 PB1.PinState=GPIO_PIN_SET。
 */

/** 使蜂鸣器发声时引脚该输出的电平。低电平触发 → RESET。 */
#define BSP_BUZZER_ACTIVE_LEVEL (GPIO_PIN_RESET)

/** 是否已初始化。 */
static bool s_ready = false;

/** 当前命令状态。 */
static bool s_on = false;

error_t bsp_buzzer_init(void)
{
    /*
     * 引脚已由 MX_GPIO_Init() 配好并置高（静音）。
     * 这里只登记状态——初始电平的唯一事实来源是 .ioc 的 PinState，
     * 在这里再写一遍就变成两份事实，将来必漏改一处。
     */
    s_on    = false;
    s_ready = true;

    return ERR_OK;
}

error_t bsp_buzzer_set(bool on)
{
    if (!s_ready) {
        return ERR_NOT_INITIALIZED;
    }

    HAL_GPIO_WritePin(BUZZER_IO_GPIO_Port, BUZZER_IO_Pin,
                      on ? BSP_BUZZER_ACTIVE_LEVEL
                         : (GPIO_PinState)!BSP_BUZZER_ACTIVE_LEVEL);

    s_on = on;

    return ERR_OK;
}

bool bsp_buzzer_is_on(void)
{
    return s_on;
}
