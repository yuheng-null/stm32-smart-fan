/**
 * @file bsp_uart.h
 * @brief 板级串口：把 interface/uart_if.h 的契约绑到具体的 USART1 上。
 *
 * 接收走**中断 + 环形缓冲**：字节由 USART1_IRQHandler 在中断里搬进缓冲，
 * 上层再从缓冲整块取走。这样收数据不会拖住主循环，也不会因为主循环
 * 忙别的（比如正在发心跳）而丢字节。
 *
 * 硬件外设本身（引脚、波特率、时钟）由 CubeMX 生成的 MX_USART1_UART_Init()
 * 初始化；本层额外负责**启动中断接收**。
 */
#ifndef BSP_UART_H
#define BSP_UART_H

#include "interface/error.h"
#include "interface/uart_if.h"

/**
 * @brief 启动串口接收。应在 MX_USART1_UART_Init() 之后调用一次。
 *
 * 做的事：打开接收中断，让字节开始被搬进环形缓冲。
 * 没有这一步，read() 永远返回 0。
 *
 * @return error_t ERR_OK 已启动；ERR_BUSY HAL 拒绝启动（外设状态不对）。
 */
error_t bsp_uart_init(void);

/**
 * @brief 取调试串口（USART1，PA9=TX / PA10=RX）的契约指针。
 *
 * 调用方拿到后只应通过 uart_if_t 的函数指针操作，不要解释 ctx。
 *
 * @return const uart_if_t* 恒不为 NULL；若将来支持多串口，
 *                           取不到时返回 NULL，调用方须判空。
 */
const uart_if_t *bsp_uart_get(void);

/**
 * @brief 因缓冲满而丢弃的字节数（累计）。
 *
 * 调试用：持续增长说明上层取数据太慢，或缓冲开小了。
 *
 * @return uint32_t 累计丢弃的字节数。
 */
uint32_t bsp_uart_get_rx_drop_count(void);

/**
 * @brief 接收错误次数（累计）：帧错误 / 噪声 / 溢出都算。
 *
 * 调试用：**接线松、没共地、波特率不匹配**时这个数会涨。
 * 本工程实测过——PA10 被外部拉低时，接收侧就是这个状态。
 *
 * @return uint32_t 累计触发出错的次数。
 */
uint32_t bsp_uart_get_rx_error_count(void);

#endif /* BSP_UART_H */
