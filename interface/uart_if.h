/**
 * @file uart_if.h
 * @brief 串口硬件抽象契约。
 *
 * 只定义契约，不含实现，也不 include 任何厂商头文件——这样 module/ 和 app/
 * 引用它时，不会顺手把 HAL 带进业务代码。
 *
 * 调用方（app/、module/）只认 uart_if_t，不知道背后是 USART1 还是别的芯片、
 * 也不知道引脚号。换板子时只改 bsp/，契约不变。
 *
 * 用函数指针而不是直接暴露句柄，是为了将来能在 PC 上做单元测试时
 * 换一份假实现（见 docs/architecture.md）。
 */
#ifndef INTERFACE_UART_IF_H
#define INTERFACE_UART_IF_H

#include <stdint.h>

#include "interface/error.h"

/**
 * @brief 发送函数签名（阻塞式）。
 *
 * @param ctx        实现方的上下文。契约把它当作不透明指针，调用方不得解释。
 * @param data       待发送数据的首地址，不得为 NULL。
 * @param len        待发送字节数，不得为 0。
 * @param timeout_ms 阻塞上限，单位毫秒。
 *
 * @return error_t ERR_OK 发送完毕；
 *                 ERR_INVALID_PARAM 指针为空或 len 为 0；
 *                 ERR_OVERFLOW len 超出本层可表达范围；
 *                 ERR_TIMEOUT 超时未发完。
 */
typedef error_t (*uart_write_fn)(void *ctx, const uint8_t *data, uint32_t len,
                                 uint32_t timeout_ms);

/**
 * @brief 接收函数签名（**非阻塞**）。
 *
 * 语义刻意设计成「取走已经有的一整块，取不到就立刻返回 0」，
 * 而不是「阻塞等 N 字节」——后者在中断驱动下是多余的，而且会强迫调用方
 * 一次只处理一个字节，白白浪费带宽（见 docs/architecture.md 的踩坑记录）。
 *
 * @param ctx     实现方的上下文，同 uart_write_fn。
 * @param data    接收缓冲首地址，不得为 NULL。
 * @param max_len 本次最多取走的字节数，不得为 0。
 *
 * @return uint32_t 实际取到的字节数：0 表示缓冲当前为空，不需要等待。
 */
typedef uint32_t (*uart_read_fn)(void *ctx, uint8_t *data, uint32_t max_len);

/** @brief 一个串口端口的完整契约。 */
typedef struct {
    uart_write_fn write;  /**< 阻塞发送 */
    uart_read_fn  read;   /**< 非阻塞取走已收到的数据 */
    void         *ctx;    /**< 实现方私有上下文，调用方不得解释 */
} uart_if_t;

#endif /* INTERFACE_UART_IF_H */
