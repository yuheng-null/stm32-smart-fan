/**
 * @file fan_curve.h
 * @brief 风扇曲线模块：温度 → 目标转速，含迟滞。
 *
 * 本文件是**纯计算**——不含 HAL、不含寄存器、不含任何硬件访问。
 * 输入是一个温度（毫摄氏度），输出是一个目标转速，因此可以在 PC 上直接做
 * 单元测试（见 tests/test_fan_curve.c）。
 *
 * ── 它在级联控制器里的位置 ───────────────────────────────────────────
 *
 *      温度 ──►[ 本模块：曲线 + 迟滞 ]──► 目标转速 ──►[ 内环 PI ]──► 占空比
 *
 *   本模块是**外环**：把「热」翻译成「要多少风」。它只输出一个**目标转速**，
 *   不碰占空比、不碰定时器——那是 bsp 与内环 PI 的事。
 *
 * ── 为什么纵向用「观测转速」而不是铭牌 RPM ────────────────────────────
 *   本工程的 rpm 是**观测转速**：由 FG 周期按「每转 2 个脉冲」换算而来
 *   （见 module/fan_control/fan_control.h）。曲线纵轴若另用铭牌 RPM，就多出
 *   一个换算系数，而这个系数还会一路传导到内环 PI 的目标——两个刻度一旦
 *   不一致，错得无声无息。
 *   统一用「观测转速」（与 fan_control_rpm_from_period_us() 同源的量）之后，
 *   曲线的纵轴与内环看到的量**同单位**，倍率误差在闭环内被消掉，只影响
 *   屏上数字的物理含义。标定之后把曲线纵轴按同一倍率缩放即可。
 *
 * ── 为什么用「分段线性插值 + 迟滞」而不是「阶梯查表」────────────────
 *   阶梯查表会让目标转速**阶跃**：温度跨过一格，目标转速突然跳一档，
 *   内环 PI 立刻去追这个阶跃，屏上的 rpm 会一跳一跳。
 *   线性插值让目标随温度连续变化，迟滞只负责挡住「在某个点附近来回抖」，
 *   正好是迟滞该干的活、不多不少。
 * ────────────────────────────────────────────────────────────────────
 */
#ifndef MODULE_FAN_CURVE_H
#define MODULE_FAN_CURVE_H

#include <stdbool.h>
#include <stdint.h>

#include "interface/error.h"

/** 曲线上的一个锚点：温度 → 目标转速。 */
typedef struct {
    int32_t  temp_milli_c;  /**< 温度，毫摄氏度 */
    uint32_t rpm;           /**< 该温度对应的目标转速（观测单位） */
} fan_curve_point_t;

/**
 * @brief 曲线配置。
 *
 * points 指向的数组由**调用者持有**，生命周期必须覆盖整机运行
 * （通常是 app 里的一个 static const 表）——本模块只读它，不复制、不释放。
 */
typedef struct {
    const fan_curve_point_t *points;    /**< 按 temp_milli_c **严格升序**，至少 2 个点 */
    uint8_t                  count;     /**< 锚点个数 */
    int32_t                  hysteresis_milli_c;  /**< 迟滞带半宽（温度单位），0 = 不迟滞 */

    /**
     * 温度低于首点 / 高于末点时输出的转速。
     *
     * 单独给两个字段而不是沿用首末点的 rpm，是为了允许「曲线外另有一套策略」：
     * 例如温度低到曲线之外时输出 0（彻底不转），而曲线首点本身是个非零的最小转速。
     * ⚠️ 它们与首末点的 rpm 不一致时，曲线在端点处会有一个**台阶**——
     *    这是调用方的选择，不是 bug，但必须是有意为之。
     */
    uint32_t rpm_at_or_below_min;
    uint32_t rpm_at_or_above_max;
} fan_curve_config_t;

/** 曲线运行状态。字段是内部实现，调用方不要直接读写。 */
typedef struct {
    fan_curve_config_t cfg;       /**< init 时复制进来的配置 */
    uint32_t           last_rpm;  /**< 上一次提交的输出，迟滞的参考点 */
    bool               primed;    /**< 是否已有过有效输出 */
} fan_curve_t;

/**
 * @brief 装载曲线。
 *
 * @param curve 待初始化的实例（调用者分配，通常 static）。
 * @param cfg   配置。会做合法性校验并**复制**进 curve（但 points 只存指针）。
 *
 * @return error_t ERR_OK 成功；
 *                 ERR_INVALID_PARAM curve/cfg/points 为空、count < 2、
 *                                   或温度锚点不是严格升序。
 */
error_t fan_curve_init(fan_curve_t *curve, const fan_curve_config_t *cfg);

/**
 * @brief 由温度求目标转速（含迟滞）。会推进 curve 内部的迟滞状态。
 *
 * 迟滞语义（在单调升的曲线上）：
 *   先求出该温度在曲线上的名义值 `nominal`；
 *   首次调用无条件提交 nominal；
 *   之后：想**升速**时，只有温度高出「上次输出对应的温度」至少
 *         `hysteresis_milli_c` 才提交，否则保持不动；想**降速**时同理、反向。
 *
 *   于是温度在某个点附近小幅抖动时，目标转速**恒定**，不会带着内环一起抖。
 *
 * @param curve        已 init 的曲线实例。
 * @param temp_milli_c 当前温度（毫摄氏度）。
 * @param target_rpm   输出，目标转速（观测单位）。
 *
 * @return error_t ERR_OK 正常；
 *                 ERR_INVALID_PARAM curve/target_rpm 为空；
 *                 ERR_NOT_INITIALIZED curve 还没 init 过。
 */
error_t fan_curve_eval(fan_curve_t *curve, int32_t temp_milli_c, uint32_t *target_rpm);

/**
 * @brief 复位迟滞状态（丢掉「上次输出」的记忆）。
 *
 * 用于切换模式、重设曲线之后：此时旧的参考点已经没有意义，
 * 若不清掉，第一次求值会被一个陈旧的参考点挡住。
 *
 * @param curve 曲线实例；为空时空操作。
 */
void fan_curve_reset(fan_curve_t *curve);

#endif /* MODULE_FAN_CURVE_H */
