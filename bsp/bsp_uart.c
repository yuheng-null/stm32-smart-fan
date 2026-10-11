/**
 * @file bsp_uart.c
 * @brief USART1 对 interface/uart_if.h 契约的实现：中断接收 + 环形缓冲。
 *
 * 为什么不是轮询：轮询版主循环里「收 1 字节 → 阻塞发 1 字节」，每轮要独占
 * 约 87 µs（115200 下 1 字节的时间），而接收寄存器只有 1 字节深——余量接近
 * 于零；一旦主循环去发别的东西（心跳），到达的字节必然溢出。实测连续收
 * 20 字节就会丢 1 个。
 *
 * 改成中断驱动后：中断只做「搬一个字节进缓冲 + 重新武装」，几微秒返回；
 * 主循环一次把攒下的整块取走。收数据不再依赖主循环的时序。
 *
 * HAL 行为依据（读 Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_uart.c 得到，
 * 不是推测）：
 *   · UART_Receive_IT() 收满后会把 RXNE / PE / ERR **三个中断全部关掉**，
 *     把 RxState 置回 READY，然后才调 RxCpltCallback。
 *     => 不重新武装就只能收到第一个字节，且不报任何错。
 *   · 溢出（ORE）被当作「阻塞性错误」：HAL_UART_IRQHandler 会调
 *     UART_EndRxTransfer() 中止整个接收，然后才调 ErrorCallback。
 *     => 错误回调里不清标志 + 不重新武装，接收就永久停在坏状态。
 */

#include "bsp/bsp_uart.h"

#include <stdint.h>

#include "main.h"

/** HAL_UART_Transmit / Receive 的 Size 参数是 uint16_t，超过即静默截断。 */
#define BSP_UART_MAX_XFER_LEN (UINT16_MAX)

/**
 * 接收环形缓冲大小。**必须是 2 的幂**——索引回绕用位与而不是取模，
 * 省掉中断里的除法周期。
 *
 * 256 字节在 115200 下约 22 ms 的缓冲深度，远大于主循环一次心跳发送
 * （约 3 ms）的量级，正常使用碰不到上限。
 */
#define BSP_UART_RX_RING_SIZE (256U)
#define BSP_UART_RX_RING_MASK (BSP_UART_RX_RING_SIZE - 1U)

/*
 * 接收环形缓冲。
 *
 * 单生产者（USART1 中断）/ 单消费者（主循环），所以**不需要临界区**：
 * 生产方只写 head，消费方只写 tail，各自读对方。
 * Cortex-M4 上 16 位读写本身是原子的，这才使无锁实现成立——换成 8 位机
 * 或改成多生产者，就必须加重入保护了。
 *
 * volatile 是必需的：这两个索引被中断和主循环同时访问，不加的话编译器
 * 可能把读操作提到循环体外做缓存，于是主循环永远看不到新数据。
 */
static volatile uint8_t  s_rx_ring[BSP_UART_RX_RING_SIZE];
static volatile uint16_t s_rx_head;  /**< 中断写：下一个写入位置 */
static volatile uint16_t s_rx_tail;  /**< 主循环写：下一个读出位置 */

/** HAL 单字节中断接收的落点，必须有固定地址，故为文件级 static。 */
static uint8_t s_rx_byte;

/** 缓冲满导致丢弃的字节数。 */
static volatile uint32_t s_rx_drop_count;

/** 接收错误回调被触发的次数（帧错误 / 噪声 / 溢出等）。 */
static volatile uint32_t s_rx_error_count;

/**
 * @brief 把 HAL 的状态码翻译成工程统一返回码。
 *
 * @param status HAL 返回的状态。
 *
 * @return error_t 对应的统一返回码。
 */
static error_t bsp_uart_map_status(HAL_StatusTypeDef status)
{
    switch (status) {
    case HAL_OK:
        return ERR_OK;
    case HAL_TIMEOUT:
        return ERR_TIMEOUT;
    case HAL_BUSY:
        return ERR_BUSY;
    default:
        return ERR_BUSY;
    }
}

/**
 * @brief 发送。实现 interface/uart_if.h 的 uart_write_fn 契约。
 */
static error_t bsp_uart_write(void *ctx, const uint8_t *data, uint32_t len,
                              uint32_t timeout_ms)
{
    UART_HandleTypeDef *huart = (UART_HandleTypeDef *)ctx;

    if ((huart == NULL) || (data == NULL) || (len == 0U)) {
        return ERR_INVALID_PARAM;
    }
    if (len > (uint32_t)BSP_UART_MAX_XFER_LEN) {
        return ERR_OVERFLOW;
    }

    return bsp_uart_map_status(
        HAL_UART_Transmit(huart, (uint8_t *)data, (uint16_t)len, timeout_ms));
}

/**
 * @brief 取走已收到的数据。实现 uart_read_fn 契约，**从不阻塞**。
 *
 * ctx 在本实现里未使用：环形缓冲是文件级单实例，写方向才需要 huart。
 */
static uint32_t bsp_uart_read(void *ctx, uint8_t *data, uint32_t max_len)
{
    uint32_t count = 0U;

    (void)ctx;

    if ((data == NULL) || (max_len == 0U)) {
        return 0U;
    }

    while ((count < max_len) && (s_rx_tail != s_rx_head)) {
        data[count] = s_rx_ring[s_rx_tail];
        s_rx_tail = (uint16_t)((s_rx_tail + 1U) & BSP_UART_RX_RING_MASK);
        count++;
    }

    return count;
}

/**
 * 调试串口的契约实例。const：绑定后不该被改写，放 .rodata 里既省 RAM
 * 也防止谁把 ctx 指到别处去。
 */
static const uart_if_t s_uart1_if = {
    .write = bsp_uart_write,
    .read  = bsp_uart_read,
    .ctx   = &huart1,
};

error_t bsp_uart_init(void)
{
    /* 打开接收中断。没有这一步，read() 永远返回 0——但不会报任何错。 */
    return bsp_uart_map_status(HAL_UART_Receive_IT(&huart1, &s_rx_byte, 1U));
}

const uart_if_t *bsp_uart_get(void)
{
    return &s_uart1_if;
}

uint32_t bsp_uart_get_rx_drop_count(void)
{
    return s_rx_drop_count;
}

uint32_t bsp_uart_get_rx_error_count(void)
{
    return s_rx_error_count;
}

/* ------------------------------------------------------------------------- *
 * HAL 回调
 *
 * 这两个是全局弱符号（HAL 里定义在 stm32f4xx_hal_uart.c 并标了 __weak），
 * 全工程只能有一份。将来若有第二个串口也走中断，在本文件里按
 * huart->Instance 分发，不要另开一份。
 * ------------------------------------------------------------------------- */

/**
 * @brief 收到 1 字节。ISR 上下文——只搬运，不做任何处理。
 *
 * 严格遵循 Fast In, Fast Out：读缓冲、写环形缓冲、重新武装，几微秒返回。
 * 协议解析、日志打印这类事一律留给主循环。
 */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    uint16_t next;

    if (huart->Instance != USART1) {
        return;
    }

    next = (uint16_t)((s_rx_head + 1U) & BSP_UART_RX_RING_MASK);
    if (next != s_rx_tail) {
        s_rx_ring[s_rx_head] = s_rx_byte;
        s_rx_head = next;
    } else {
        /* 缓冲满：丢新字节，不覆盖老数据。老数据通常正在被上层处理。 */
        s_rx_drop_count++;
    }

    /* 必须重新武装——否则只能收到这一个字节（见文件头的 HAL 行为依据） */
    if (HAL_UART_Receive_IT(huart, &s_rx_byte, 1U) != HAL_OK) {
        s_rx_error_count++;
    }
}

/**
 * @brief 接收出错。ISR 上下文。
 *
 * 典型触发场景是拔插一次 USB-TTL——线上的噪声脉冲被判成帧错误。
 * HAL 已经把接收中止了（见文件头），所以这里是把它救回来的唯一机会：
 * 不清标志又不重新武装，之后**永久只能发不能收**。
 */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance != USART1) {
        return;
    }

    s_rx_error_count++;

    /* 清 PE/FE/NE/ORE：HAL 的宏是「先读 SR 再读 DR」这个序列，
       这几个标志必须按这个顺序才清得掉。 */
    __HAL_UART_CLEAR_PEFLAG(huart);

    /*
     * 只在 HAL 确实把接收中止了的时候重新武装（RxState 回了 READY）。
     *
     * 理由在 HAL_UART_IRQHandler 的处理顺序里（本机 HAL 源码）：
     *   · **非阻塞错误**（FE/NE/PE，不含 ORE）：先因 RXNE 调 UART_Receive_IT()，
     *     于是 HAL_UART_RxCpltCallback() 已经跑过、并重新武装好了
     *     （RxState 仍是 BUSY）；随后才调本回调。此时再武装一次只会拿到
     *     HAL_BUSY——若把它当失败计数，**一次帧错误会让 rx_err 涨 2**。
     *     而这个计数正是文档里推荐用来看「接线松没松、有没有共地」的指标，
     *     翻倍会让它失去参考价值。
     *   · **阻塞错误**（ORE）：HAL 先 UART_EndRxTransfer()（RxState 回 READY），
     *     且**不会**走到 RxCplt，所以这一支必须由这里重新武装——否则之后
     *     「永久只能发不能收」。
     */
    if (huart->RxState == HAL_UART_STATE_READY) {
        if (HAL_UART_Receive_IT(huart, &s_rx_byte, 1U) != HAL_OK) {
            s_rx_error_count++;
        }
    }
}
