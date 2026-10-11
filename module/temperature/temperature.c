/**
 * @file temperature.c
 * @brief 温度换算的实现（NTC 分压 -> Beta 方程 -> 摄氏温度）。
 *
 * 硬件模型与参数出处见 temperature.h 顶部。
 */

#include "module/temperature/temperature.h"

#include <math.h>
#include <stddef.h>

/** 绝对零度对应的摄氏温度。 */
#define TEMPERATURE_KELVIN_TO_CELSIUS   (273.15f)

/** NTC 阻值为 0 或无穷大时的替代值：算出来的端点温度即这两个极限。 */
#define TEMPERATURE_RAW_MIN_AS_R_OHM    (1.0f)

uint32_t temperature_resistance_from_raw(uint16_t raw)
{
    if ((raw < TEMPERATURE_RAW_MIN) || (raw > TEMPERATURE_RAW_MAX)) {
        return 0U;
    }

    /*
     * R_NTC = R_FIXED · raw / (4095 − raw)
     *
     * 用 float 算：分子分母都是整数，整数除法会把比例截断成 0 或 1，
     * 1000 计数附近的分辨率会直接丢掉。Cortex-M4 有单精度 FPU，
     * float 除法是几条指令的事。
     */
    const float ratio = (float)raw / (float)(TEMPERATURE_ADC_FULL_SCALE - (int32_t)raw);

    return (uint32_t)(TEMPERATURE_DIVIDER_R_OHM * ratio);
}

error_t temperature_from_adc_raw(uint16_t raw, int32_t *milli_c)
{
    float r_ntc;
    float temp_k;
    error_t status = ERR_OK;

    if (milli_c == NULL) {
        return ERR_INVALID_PARAM;
    }

    if (raw < TEMPERATURE_RAW_MIN) {
        /*
         * AO 被拉到地：要么 NTC 短路，要么分压电阻开路。
         * 数学上 R_NTC → 0，Beta 方程里 ln 发散；物理上这是「极热」的极限，
         * 所以钳到公式能表达的端点，同时把状态如实报上去，
         * 让上层有机会显示「---」而不是一个看起来正常的读数。
         */
        r_ntc  = TEMPERATURE_RAW_MIN_AS_R_OHM;
        status = ERR_UNDERFLOW;
    } else if (raw > TEMPERATURE_RAW_MAX) {
        /* AO 被拉到 VCC：NTC 开路。极限是「极冷」，同样钳住并上报。 */
        r_ntc  = TEMPERATURE_DIVIDER_R_OHM * (float)TEMPERATURE_ADC_FULL_SCALE;
        status = ERR_OVERFLOW;
    } else {
        const float ratio = (float)raw / (float)(TEMPERATURE_ADC_FULL_SCALE - (int32_t)raw);
        r_ntc = TEMPERATURE_DIVIDER_R_OHM * ratio;
    }

    /*
     * Beta 方程：1/T = 1/T0 + ln(R/R0) / B
     *
     * 先把 1/T 算出来再取倒数，比先解 T 再除要少一次除法，也更贴合公式。
     */
    const float inv_t = (1.0f / TEMPERATURE_NTC_T0_K)
                      + (logf(r_ntc / TEMPERATURE_NTC_R0_OHM) / TEMPERATURE_NTC_B);

    temp_k = 1.0f / inv_t;

    /*
     * 乘 1000 转成毫摄氏度。用 lroundf 四舍五入，不要直接截断：
     * 截断会让负温度整体偏低 1 个单位（往 -∞ 方向），显示上看得见。
     */
    *milli_c = (int32_t)lroundf((temp_k - TEMPERATURE_KELVIN_TO_CELSIUS) * 1000.0f);

    return status;
}
