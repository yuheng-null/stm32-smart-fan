/**
 * @file bsp_ir.c
 * @brief 板级红外接收头的实现（PC7 / EXTI7，双边沿）。
 *
 * 分工与接线见 bsp_ir.h 顶部。这里只说明缓冲与中断的几处细节。
 *
 * ── 为什么是一格空的环形缓冲 ────────────────────────────────────────
 *   「满」与「空」都表现为 head == tail，分不开。多留一格不用（容量 N 只放
 *   N−1 个），就可以用「下一个 head 会不会撞上 tail」来判满。代价是一个格子，
 *   换来的是判定不需要额外的计数器——也就没有「计数器和指针不同步」这种 bug。
 *
 * ── 为什么不需要临界区 ──────────────────────────────────────────────
 *   单生产者（EXTI 中断）单消费者（主循环）：生产者只改 head，消费者只改
 *   tail。两边各自读对方那个只读，且都是 32 位对齐访问（Cortex-M4 上原子）。
 *   唯一要小心的是**先后顺序**：生产者必须先把数据写进去、再更新 head，
 *   否则消费者可能读到一个「已可见但还没写」的槽位。下面的代码就按这个顺序。
 */

#include "bsp/bsp_ir.h"

#include <stdbool.h>
#include <stddef.h>

#include "main.h"                   /* 借它引入 HAL 头与 IR_RX_GPIO_Port / IR_RX_Pin */

#include "bsp/bsp_time.h"

/**
 * 环形缓冲的槽位数。
 *
 * 一帧 NEC 是 68 个边沿；主循环 100 ms 取一次，而按住不放时帧间隔约 110 ms，
 * 所以一个周期里通常只有一帧多一点的量。取 192 就留了近三帧的余量——
 * 主循环被 OLED 整屏刷新（几十毫秒）挡一下也不会丢。
 */
#define BSP_IR_RING_SIZE    (192U)

/** 槽位数组。数据只由中断写、只由主循环读。 */
static volatile bsp_ir_edge_t s_ring[BSP_IR_RING_SIZE];

/** 写入位置（中断改）。 */
static volatile uint32_t s_head = 0U;

/** 读取位置（主循环改）。 */
static volatile uint32_t s_tail = 0U;

/** 因缓冲满被丢掉的边沿数。 */
static volatile uint32_t s_dropped = 0U;

/** 是否已经初始化过。 */
static bool s_ready = false;

/* ----------------------------------------------------------------- 内部 */

/**
 * @brief 入队一个边沿。**中断上下文**。
 *
 * ⚠️ 顺序要紧：先写槽位、再抬 head。反过来的话，消费者可能看到一个 head
 *    已经前进、但槽位内容还没写的状态。
 */
static void ir_ring_push(const bsp_ir_edge_t *edge)
{
    const uint32_t next = (s_head + 1U) % BSP_IR_RING_SIZE;

    if (next == s_tail) {
        /* 满了。丢**新的**那个：旧数据是这一帧的前半段，丢了它整帧都废；
           而丢掉新边沿至少还有机会在下一个引导码重新锁上。 */
        s_dropped++;
        return;
    }

    s_ring[s_head] = *edge;
    s_head         = next;
}

/* ----------------------------------------------------------------- 对外 */

error_t bsp_ir_init(void)
{
    s_head    = 0U;
    s_tail    = 0U;
    s_dropped = 0U;
    s_ready   = false;

    /*
     * 时基就绪检查：TIM2 的计数器是在 bsp_fan_init() 里启动的，在那之前
     * bsp_time_counter_hz() 虽然能算出频率，但计数器恒为 0，边沿时刻全是 0，
     * 解码必然全错——而且不报任何错。宁可在这里拦住。
     */
    if (bsp_time_counter_hz() == 0U) {
        return ERR_NOT_INITIALIZED;
    }

    s_ready = true;

    return ERR_OK;
}

bool bsp_ir_take_edge(bsp_ir_edge_t *out)
{
    if ((out == NULL) || (s_tail == s_head)) {
        return false;
    }

    /*
     * 先读数据、再抬 tail：万一中断恰好在两者之间推进了 head，那也只是让
     * 下一次能多取一个，不会让本次取到一个还没写完的槽位——因为 head 前进
     * 意味着那个槽位已经写完了。
     */
    *out   = s_ring[s_tail];
    s_tail = (s_tail + 1U) % BSP_IR_RING_SIZE;

    return true;
}

uint32_t bsp_ir_get_dropped(void)
{
    return s_dropped;
}

void bsp_ir_flush(void)
{
    s_tail = s_head;
}

/**
 * @brief EXTI 回调。**中断上下文，由 HAL_GPIO_EXTI_IRQHandler 调用。**
 *
 * 生成文件（Core/Src/stm32f4xx_it.c）里的 EXTI9_5_IRQHandler 已经先清掉了
 * 挂起位再进来，所以这里不必再清。
 *
 * 只做三件事：确认是本引脚、读电平与时刻、入队。任何解析都不在这里做。
 */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    if ((GPIO_Pin != IR_RX_Pin) || !s_ready) {
        return;
    }

    bsp_ir_edge_t edge;

    /* 边沿**之后**的电平：此刻中断已进入，引脚电平已经稳定在新状态上。 */
    edge.level = (HAL_GPIO_ReadPin(IR_RX_GPIO_Port, IR_RX_Pin) == GPIO_PIN_SET);
    edge.us    = bsp_time_us();

    ir_ring_push(&edge);
}
