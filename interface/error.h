/**
 * @file error.h
 * @brief 全工程统一的返回码。
 *
 * 放在 interface/ 是因为它是「契约」：app/ 和 module/ 都依赖它，
 * 而它不依赖任何硬件，也不 include 任何厂商头文件。
 *
 * 不用裸 -1 / -2 之类的魔法返回值：调用方无法从 -1 判断到底错在哪。
 */
#ifndef INTERFACE_ERROR_H
#define INTERFACE_ERROR_H

typedef enum {
    ERR_OK = 0,           /**< 成功 */
    ERR_TIMEOUT,          /**< 等待超时（数据没来 / 外设没响应） */
    ERR_BUSY,             /**< 外设忙，本次操作未完成 */
    ERR_INVALID_PARAM,    /**< 入参非法（空指针、越界、长度为 0） */
    ERR_NOT_INITIALIZED,  /**< 依赖的层还没初始化 */
    ERR_OVERFLOW,         /**< 数据或长度超出本层能表达的范围 */
    ERR_UNDERFLOW,        /**< 数据不足 */
} error_t;

#endif /* INTERFACE_ERROR_H */
