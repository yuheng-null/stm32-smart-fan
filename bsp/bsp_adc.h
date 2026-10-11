/**
 * @file bsp_adc.h
 * @brief 板级 ADC：NTC 模块的模拟输出（AO）。
 *
 * ── 接线 ────────────────────────────────────────────────────────────
 *   NTC 模块 AO ── PA0      （PA0 = ADC1_IN0）
 *   NTC 模块 VCC ── 3V3     ⚠️ 必须 3.3V，理由见下
 *   NTC 模块 GND ── GND
 *
 *   ⚠️ 为什么 VCC 必须是 3.3V 而不是 5V：
 *     ① **量程**：ADC 的参考电压 VREF+ 是 3.3V。模块若用 5V 供电，
 *        AO 会输出 0~5V，高过 3.3V 的部分全部被削平在满量程，
 *        温度偏低的那一大段读数完全一样，等于没有区分度。
 *     ② **安全**：PA0 在数据手册的引脚表里标着 FT（5V 耐受），
 *        但那条脚注写的是「FT = 5 V tolerant **except when in analog mode**」。
 *        PA0 一旦被配成 ADC 的模拟输入就不在 5V 耐总之列，
 *        输入上限是 VDDA（3.6V 绝对最大）。5V 会经保护二极管倒灌。
 *
 *   两路电源都取自同一块板子的 3.3V，因此模块的 VCC 与 ADC 的 VREF+
 *   是同一条轨——这一点被 module/temperature 用上了：换算电阻时
 *   VCC 与 VREF 相消，电源的绝对精度不影响结果。
 * ────────────────────────────────────────────────────────────────────
 *
 * ── 关于校准 ────────────────────────────────────────────────────────
 *   STM32F1/F3 的 ADC 有硬件自校准（CAL 位），HAL 里是
 *   HAL_ADCEx_Calibration_Start()，漏掉会带来几十个 LSB 的固定偏差。
 *   **STM32F4 没有这个功能**：ADC_CR2 里没有 ADCAL 位，F4 的 HAL 里
 *   也不存在 HAL_ADCEx_Calibration_Start()（已在本机
 *   Drivers/STM32F4xx_HAL_Driver/ 下确认：该符号全库不存在）。
 *   所以本层不做校准——不是漏了，是这颗芯片没有。
 * ────────────────────────────────────────────────────────────────────
 */
#ifndef BSP_ADC_H
#define BSP_ADC_H

#include <stdint.h>

#include "interface/error.h"

/**
 * @brief 初始化 ADC，使其可以开始转换。
 *
 * 外设本身的参数（12 位、右对齐、单次转换、序列长度 1、采样时间 480 周期）
 * 由 CubeMX 生成的 MX_ADC1_Init() 配置，本函数只做「让它可转换」这件事。
 *
 * @return error_t ERR_OK 就绪；ERR_NOT_READY 外设状态不对。
 *
 * @note 必须在 MX_ADC1_Init() 之后调用。
 */
error_t bsp_adc_init(void);

/**
 * @brief 采一次，取回原始计数（0~4095）。
 *
 * @param raw 输出，原始转换值。
 *
 * @return error_t ERR_OK 成功；ERR_INVALID_PARAM raw 为空；
 *                 ERR_NOT_INITIALIZED 尚未初始化；
 *                 ERR_TIMEOUT 转换超时（近似说明 ADC 时钟没开）。
 */
error_t bsp_adc_read_raw(uint16_t *raw);

/**
 * @brief 连续采 count 次求平均，压低随机噪声。
 *
 * 单次转换的噪声通常有 ±2~3 LSB，直接拿去算温度会让显示末位一直跳。
 * 求平均能把噪声按 1/√count 压下去。
 *
 * @param out   输出，平均值（四舍五入到整数计数）。
 * @param count 采样次数，建议 16；为 0 视为 1。
 *
 * @return error_t 见 bsp_adc_read_raw()；任何一次失败即整体失败。
 */
error_t bsp_adc_read_averaged(uint16_t *out, uint16_t count);

#endif /* BSP_ADC_H */
