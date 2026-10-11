/**
 * @file bsp_i2c.h
 * @brief 板级软件 I2C 总线：本板唯一一条（OLED 用）。
 *
 * ── 接线 ────────────────────────────────────────────────────────────
 *   OLED SCL ── PB6        OLED SDA ── PB7
 *
 *   PB6/PB7 在 CubeMX 里配成**开漏输出**（GPIO_Output + Output OD），
 *   引脚标签为 OLED_SCL / OLED_SDA，于是 main.h 里生成
 *   OLED_SCL_Pin / OLED_SCL_GPIO_Port 这组宏。
 *
 *   总线的上拉来自 OLED 模块**板载**的 4.7 kΩ 电阻；
 *   芯片内部约 40 kΩ 的上拉只作兜底（详见 driver/bus/i2c/i2c_soft.c）。
 * ────────────────────────────────────────────────────────────────────
 *
 * 单独成模块而不是塞进 bsp_oled：I2C 是**总线**，OLED 只是挂在上面的一个
 * 从机。将来再挂一颗 I2C 温度传感器时，只需新增一个 device 驱动，
 * 总线这一份不用动。
 */
#ifndef BSP_I2C_H
#define BSP_I2C_H

#include "interface/error.h"
#include "interface/i2c_if.h"

/**
 * @brief 初始化软件 I2C 总线（PB6=SCL / PB7=SDA）。
 *
 * @return error_t ERR_OK 成功；ERR_INVALID_PARAM 配置非法。
 *
 * @note 必须在 MX_GPIO_Init() **之后**调用——GPIOB 的时钟在那里使能。
 *       本函数内部会再配一遍引脚电气属性（开漏 + 上拉），
 *       与 CubeMX 生成的配置保持一致。
 */
error_t bsp_i2c_init(void);

/**
 * @brief 取本板软件 I2C 总线的契约指针。
 *
 * @return i2c_if_t* 未初始化时返回 NULL；调用方须判空。
 */
i2c_if_t *bsp_i2c_get(void);

/**
 * @brief 总线卡死恢复。
 *
 * 从机在传输中途复位（例如 OLED 被拔插一次）可能一直拉住 SDA，
 * 之后所有起始条件都失效。本函数补发 9 个时钟把它救回来。
 *
 * @return error_t ERR_OK 总线已释放；ERR_BUSY 仍被拉低；ERR_NOT_INITIALIZED 未初始化。
 */
error_t bsp_i2c_recover(void);

#endif /* BSP_I2C_H */
