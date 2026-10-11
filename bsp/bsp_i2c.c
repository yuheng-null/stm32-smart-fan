/**
 * @file bsp_i2c.c
 * @brief 板级软件 I2C 总线实现（把 driver/bus/i2c/i2c_soft 绑到 PB6/PB7）。
 */

#include "bsp/bsp_i2c.h"

#include <stdbool.h>

#include "main.h"                   /* 借它引入 stm32f4xx_hal.h 与引脚宏 */

#include "driver/bus/i2c/i2c_soft.h"

/** 本板唯一一条软件 I2C 总线。板级资源天然只有一份，故用文件级静态。 */
static i2c_soft_t s_bus;

/** 是否已初始化，供取接口时判断前置条件。 */
static bool s_ready = false;

error_t bsp_i2c_init(void)
{
    /*
     * 引脚宏来自 CubeMX（main.h），是「PB6 接 SCL、PB7 接 SDA」这件事的
     * 唯一出处。改接线要改 .ioc（走 cube-recipes/），不要改这里。
     */
    const i2c_soft_cfg_t cfg = {
        .scl_port = OLED_SCL_GPIO_Port,
        .scl_pin  = OLED_SCL_Pin,
        .sda_port = OLED_SDA_GPIO_Port,
        .sda_pin  = OLED_SDA_Pin,
        .freq_hz  = I2C_SOFT_DEFAULT_FREQ_HZ,
    };

    s_ready = false;

    const error_t err = i2c_soft_init(&s_bus, &cfg);
    if (err != ERR_OK) {
        return err;
    }

    s_ready = true;

    return ERR_OK;
}

i2c_if_t *bsp_i2c_get(void)
{
    if (!s_ready) {
        return NULL;
    }

    return i2c_soft_iface(&s_bus);
}

error_t bsp_i2c_recover(void)
{
    if (!s_ready) {
        return ERR_NOT_INITIALIZED;
    }

    return i2c_soft_bus_recover(&s_bus);
}
