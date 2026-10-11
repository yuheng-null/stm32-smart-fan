/**
 * @file    i2c_if.h
 * @brief   抽象 I2C 总线接口。
 *
 * 设备驱动（如 ssd1306）只允许通过本接口访问总线，
 * 不允许出现 I2C_HandleTypeDef、HAL_I2C_* 等 STM32 专有符号。
 * 与 uart_if.h 一样放在 interface/：这里只放**契约**，不放任何实现。
 * 这样换 MCU 平台时只需要换一份总线实现，设备驱动一行不用改；
 * 在 PC 上做单元测试时也可以注入一个 mock 实现。
 */

#ifndef DRIVER_BUS_I2C_IF_H
#define DRIVER_BUS_I2C_IF_H

#include <stdint.h>

#include "interface/error.h"

/**
 * @brief I2C 主机总线接口（7 位地址）。
 *
 * 三个成员函数都带 ctx，指向具体的总线实例，因此同一份实现可以
 * 支持多条总线，也支持总线实例的多份并存。
 */
typedef struct {
    /**
     * @brief 向从机写数据。
     * @param ctx  总线实例
     * @param addr 从机 7 位地址（不含读写位）
     * @param data 待发送数据
     * @param len  数据长度，不得为 0
     */
    error_t (*write)(void *ctx, uint8_t addr, const uint8_t *data, uint32_t len);

    /**
     * @brief 从从机读数据。
     * @param ctx  总线实例
     * @param addr 从机 7 位地址（不含读写位）
     * @param data 接收缓冲
     * @param len  期望读取长度，不得为 0
     */
    error_t (*read)(void *ctx, uint8_t addr, uint8_t *data, uint32_t len);

    /**
     * @brief 先写后读，中间用重复起始条件衔接（寄存器读的典型时序）。
     * @param ctx     总线实例
     * @param addr    从机 7 位地址（不含读写位）
     * @param cmd     先写出的内容（通常是要读的寄存器地址）
     * @param cmd_len 写长度，不得为 0
     * @param rx      接收缓冲
     * @param rx_len  读长度，不得为 0
     */
    error_t (*write_read)(void *ctx, uint8_t addr,
                          const uint8_t *cmd, uint32_t cmd_len,
                          uint8_t *rx, uint32_t rx_len);

    void *ctx;  /**< 具体总线实例，由实现方填充 */
} i2c_if_t;

#endif /* DRIVER_BUS_I2C_IF_H */
