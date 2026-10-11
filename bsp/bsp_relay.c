/**
 * @file bsp_relay.c
 * @brief 板级继电器实现（PB0，高电平触发）。
 */

#include "bsp/bsp_relay.h"

#include <stdbool.h>

#include "main.h"                   /* 借它引入 HAL 头与 RELAY_IN_Pin / _GPIO_Port */

/*
 * 引脚的配置（推挽输出、初始低）在 CubeMX 里，不在本文件——
 * 见 .ioc 的 PB0.GPIO_Label=RELAY_IN。理由同 bsp_key.c。
 */

/** 使继电器吸合时引脚该输出的电平。高电平触发 → SET。 */
#define BSP_RELAY_ACTIVE_LEVEL  (GPIO_PIN_SET)

/** 是否已初始化。 */
static bool s_ready = false;

/** 当前命令状态。 */
static bool s_on = false;

error_t bsp_relay_init(void)
{
    /*
     * 引脚已由 MX_GPIO_Init() 配好并置低（断开）。
     * 这里只登记状态——不要在这里再写一次 GPIO 电平：
     * 那会与 .ioc 里的初始电平变成两份事实，改动时必漏一处。
     */
    s_on    = false;
    s_ready = true;

    return ERR_OK;
}

error_t bsp_relay_set(bool on)
{
    if (!s_ready) {
        return ERR_NOT_INITIALIZED;
    }

    HAL_GPIO_WritePin(RELAY_IN_GPIO_Port, RELAY_IN_Pin,
                      on ? BSP_RELAY_ACTIVE_LEVEL : (GPIO_PinState)!BSP_RELAY_ACTIVE_LEVEL);

    s_on = on;

    return ERR_OK;
}

bool bsp_relay_is_on(void)
{
    return s_on;
}
