/**
 * @file bsp_time.c
 * @brief 系统微秒时基的实现：TIM2 的自由运行计数器。
 *
 * 为什么复用 TIM2、使用前提是什么，都写在 bsp_time.h 顶部。
 */

#include "bsp/bsp_time.h"

#include "main.h"                   /* 借它引入 HAL 头与 htim2 */

/**
 * @brief TIM2 的计数频率（Hz）。见头文件。
 *
 * ⚠️ 定时器时钟**不等于** PCLK：APB 预分频 = 1 时等于 PCLKx，
 *    预分频 > 1 时是 **2 × PCLKx**（STM32 时钟树里的一个例外，
 *    不查手册很容易把 TIM2 当成 42 MHz 而算错一倍）。
 */
uint32_t bsp_time_counter_hz(void)
{
    const uint32_t pclk1 = HAL_RCC_GetPCLK1Freq();

    /* RM0090 时钟树：PPRE1 = 1 时定时器时钟 = PCLK1，否则 = 2 × PCLK1 */
    const uint32_t tim_clk = ((RCC->CFGR & RCC_CFGR_PPRE1) == RCC_CFGR_PPRE1_DIV1)
                                 ? pclk1
                                 : (pclk1 * 2U);

    /* HAL 在 HAL_TIM_IC_Init() 里把 Init.Prescaler 写进了 TIM2->PSC，两者恒等。
       （HAL 只给了 __HAL_TIM_SET_PRESCALER，没有 GET——别照着 SET 的名字去猜。） */
    const uint32_t psc = (uint32_t)htim2.Init.Prescaler;

    return tim_clk / (psc + 1U);
}

uint32_t bsp_time_us(void)
{
    const uint32_t hz = bsp_time_counter_hz();

    if (hz == 0U) {
        return 0U;
    }

    const uint32_t counts = __HAL_TIM_GET_COUNTER(&htim2);

    /* 本板 hz 恰好是 1 MHz，除法是恒等变换；留着它是为了**改 PSC 后依然正确**。
       用 64 位中间量，免得 hz 较大时 counts * 1000000 溢出。 */
    return (uint32_t)(((uint64_t)counts * 1000000ULL) / (uint64_t)hz);
}
