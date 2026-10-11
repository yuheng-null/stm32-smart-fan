/**
 * @file    i2c_soft.h
 * @brief   软件模拟（位翻转）I2C 主机总线 —— STM32F4 实现。
 *
 * 为什么用软件模拟而不是硬件 I2C：
 *   1) 本工程的 OLED 接在 PB6/PB7，正好就是 I2C1 的默认引脚——用硬件外设
 *      本可以，但软件模拟不受引脚限制，改接线时不用动 CubeMX；
 *   2) 位翻转把起止条件、应答位、时序都摆在明面上，便于理解 I2C 协议本身，
 *      出问题时逻辑分析仪一抓就能对上。
 *
 * 依赖说明：本文件属于「总线驱动」层，是直接贴着 HAL 的一层，
 * 允许使用 HAL_GPIO_*；设备驱动层（driver/device/）则不允许（见 i2c_if.h）。
 *
 * 引脚不写死在本文件里，由 bsp/ 通过 i2c_soft_cfg_t 注入——
 * 这样同一份实现可以驱动任意两根 GPIO。
 */

#ifndef DRIVER_BUS_I2C_SOFT_H
#define DRIVER_BUS_I2C_SOFT_H

#include <stdbool.h>
#include <stdint.h>

#include "main.h"                   /* 借它引入 stm32f4xx_hal.h */

#include "interface/error.h"
#include "interface/i2c_if.h"

/** 目标 SCL 频率（Hz）。软件模拟下实际频率受 GPIO 翻转开销影响，会略低于此值。 */
#define I2C_SOFT_DEFAULT_FREQ_HZ    (400000U)

/** 总线恢复时补发的时钟个数（I2C 规定 9 个，足以打空从机的位计数器） */
#define I2C_SOFT_RECOVER_CLOCKS     (9U)

/**
 * @brief 软件 I2C 的引脚配置。
 *
 * SCL/SDA 允许在不同端口上，因此端口与引脚分开给出。
 */
typedef struct {
    GPIO_TypeDef *scl_port;         /**< SCL 所在端口 */
    uint16_t      scl_pin;          /**< SCL 引脚（GPIO_PIN_x） */
    GPIO_TypeDef *sda_port;         /**< SDA 所在端口 */
    uint16_t      sda_pin;          /**< SDA 引脚（GPIO_PIN_x） */
    uint32_t      freq_hz;          /**< 目标 SCL 频率，填 0 则用 I2C_SOFT_DEFAULT_FREQ_HZ */
} i2c_soft_cfg_t;

/**
 * @brief 软件 I2C 总线实例。
 *
 * 调用者只需持有本结构体，不要直接访问其内部字段，
 * 对外的访问一律通过 i2c_soft_iface() 取到的 i2c_if_t。
 */
typedef struct {
    i2c_soft_cfg_t cfg;         /**< 引脚与频率 */
    i2c_if_t       iface;       /**< 对外的抽象接口 */
    uint32_t       delay_us;    /**< 半周期延时（微秒） */
    bool           ready;       /**< 初始化完成标志 */
} i2c_soft_t;

/**
 * @brief 初始化软件 I2C 总线：配置 SCL/SDA 为开漏输出并拉高。
 *
 * @param bus 总线实例（由调用者分配，通常是静态存储期）
 * @param cfg 引脚与频率配置，函数内会拷贝一份
 *
 * @return error_t ERR_OK 成功；ERR_INVALID_PARAM 指针为空
 *
 * @note 调用前必须已使能对应 GPIO 端口的时钟（由 BSP 层负责）。
 */
error_t i2c_soft_init(i2c_soft_t *bus, const i2c_soft_cfg_t *cfg);

/**
 * @brief 取出该总线的抽象接口。
 *
 * @param bus 已初始化的总线实例
 *
 * @return i2c_if_t* 接口指针；入参为空或未初始化时返回 NULL
 */
i2c_if_t *i2c_soft_iface(i2c_soft_t *bus);

/**
 * @brief 总线卡死恢复。
 *
 * 从机在传输中途复位时可能一直拉低 SDA，导致后续起始条件失效。
 * 本函数在 SCL 上补发 9 个时钟把从机的位计数器打空，再补一个停止条件。
 *
 * @param bus 已初始化的总线实例
 *
 * @return error_t ERR_OK 总线已释放（SDA 回到高）；ERR_BUSY 仍被拉低
 */
error_t i2c_soft_bus_recover(i2c_soft_t *bus);

#endif /* DRIVER_BUS_I2C_SOFT_H */
