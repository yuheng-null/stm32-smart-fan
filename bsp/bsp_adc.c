/**
 * @file bsp_adc.c
 * @brief 板级 ADC 实现（ADC1 通道 0 ← PA0）。
 */

#include "bsp/bsp_adc.h"

#include <stdbool.h>
#include <stddef.h>

#include "main.h"                   /* 借它引入 stm32f4xx_hal.h 与 hadc1 声明 */

/*
 * ── 为什么每次采样都 Start/Stop，而不是让它一直在跑 ──────────────────
 *   本工程一秒才采一次温度，没有连续采样的需求。每次 Start/Stop 换来的是
 *   「转换不在后台乱跑」这个明确状态，代价只是几条寄存器操作，可以忽略。
 *
 * ── 为什么用 HAL_ADC_PollForConversion ───────────────────────────────
 *   本机 Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_adc.c 里能看到
 *   它的实现：**无条件**死等 EOC 标志（F1 版本还有一条「扫描模式下不查标志、
 *   只空转最坏时间」的分支，F4 这版没有）。所以它在单通道轮询下是可靠的。
 *   它自带超时，比手写自旋计数好在「ADC 时钟忘了开」时能返回错误而不是挂死。
 * ────────────────────────────────────────────────────────────────────
 */

/** 单次转换的等待上限（毫秒）。480 周期 @21 MHz 只要约 23 µs，给 5 ms 极宽松。 */
#define BSP_ADC_TIMEOUT_MS  (5U)

/** 是否已初始化。 */
static bool s_ready = false;

error_t bsp_adc_init(void)
{
    s_ready = false;

    /*
     * 这里没有「校准」这一步，不是漏了：STM32F4 的 ADC 没有硬件自校准
     * （ADC_CR2 无 ADCAL 位），HAL 里也不存在对应的函数。
     * 详见 bsp_adc.h 顶部的说明。
     *
     * 外设参数由 CubeMX 生成的 MX_ADC1_Init() 写入，本函数只需标记就绪。
     */
    s_ready = true;

    return ERR_OK;
}

error_t bsp_adc_read_raw(uint16_t *raw)
{
    if (raw == NULL) {
        return ERR_INVALID_PARAM;
    }
    if (!s_ready) {
        return ERR_NOT_INITIALIZED;
    }

    if (HAL_ADC_Start(&hadc1) != HAL_OK) {
        return ERR_NOT_READY;
    }

    if (HAL_ADC_PollForConversion(&hadc1, BSP_ADC_TIMEOUT_MS) != HAL_OK) {
        (void)HAL_ADC_Stop(&hadc1);
        return ERR_TIMEOUT;
    }

    /* 读 DR 取回结果，同时硬件自动清掉 EOC */
    *raw = (uint16_t)HAL_ADC_GetValue(&hadc1);

    (void)HAL_ADC_Stop(&hadc1);

    return ERR_OK;
}

error_t bsp_adc_read_averaged(uint16_t *out, uint16_t count)
{
    uint32_t sum = 0U;

    if (out == NULL) {
        return ERR_INVALID_PARAM;
    }

    if (count == 0U) {
        count = 1U;
    }

    for (uint16_t i = 0U; i < count; i++) {
        uint16_t      value = 0U;
        const error_t err   = bsp_adc_read_raw(&value);

        if (err != ERR_OK) {
            return err;         /* 任何一次失败即整体失败，不拿残缺的平均值去用 */
        }

        sum += value;
    }

    /* 四舍五入：先加半个步长再整除，避免平均值系统性偏小。 */
    *out = (uint16_t)((sum + ((uint32_t)count / 2U)) / (uint32_t)count);

    return ERR_OK;
}
