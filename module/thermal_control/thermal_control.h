/**
 * @file thermal_control.h
 * @brief 温控级联控制器：温度 → 目标转速 → 占空比。
 *
 * 纯计算，不含 HAL、不含寄存器。输入是两次采样（温度、转速）与当前时刻，
 * 输出是一个占空比与一组报警位，因此可以在 PC 上整条链做闭环仿真
 * （见 tests/test_thermal_control.c）。
 *
 * ── 为什么是级联，而不是「温度直接算占空比」──────────────────────────
 *
 *      温度 ──►[ 曲线 + 迟滞 ]──► 目标转速 ──►[ PI ]──► 占空比 ──► 风扇
 *                    (外环)                      (内环)         │
 *                                                                │
 *                       实测转速 ◄──────── FG 捕获 ◄─────────────┘
 *
 *   两个理由：
 *   ① **风扇的转速-占空比关系是非线性的**（低压区还有一段行为未定义的死区，
 *      见 fan_control.h）。直接把温度映射成占空比，等于把这段非线性塞进外环，
 *      整定会随工作点漂移；闭到转速上之后，内环把这段非线性「吃」掉了。
 *   ② **对扰动免疫**。电源电压波动、进风口被挡、滤网积灰，都会改变
 *      「同样的占空比能吹出多少风」。闭到转速上，这些扰动由内环吸收，
 *      外环看到的对象始终是线性的。
 *
 * ── 对两个「未标定」的免疫 ───────────────────────────────────────────
 *   · rpm 绝对倍率未标定（每转脉冲数可能 ≠ 2）：曲线纵轴与内环的实测值
 *     **同单位**，倍率误差在闭环内被消掉，只影响显示数字的物理含义。
 *   · NTC 参数未校准：影响的是曲线**横轴**（温度），因此它会平移工作点。
 *     这是为什么本模块把偏移量暴露给用户（串口 TEMP / 按键），而不是把
 *     某个绝对温度焊死在代码里。
 * ────────────────────────────────────────────────────────────────────
 */
#ifndef MODULE_THERMAL_CONTROL_H
#define MODULE_THERMAL_CONTROL_H

#include <stdbool.h>
#include <stdint.h>

#include "interface/error.h"
#include "module/common/control_types.h"
#include "module/fan_curve/fan_curve.h"
#include "module/pi_control/pi_control.h"

/** 控制器配置。 */
typedef struct {
    fan_curve_config_t curve;               /**< 外环曲线（含迟滞） */
    pi_config_t        pi;                  /**< 内环 PI 参数 */

    uint32_t control_period_ms;             /**< 标称控制周期，用作 dt 的兜底值 */

    uint32_t fallback_rpm;                  /**< 温度失效时的目标转速（安全偏高速） */

    /**
     * 传感器去抖次数。
     *
     * 连续 invalid_debounce_samples 次无效才进兜底，连续 recovery_debounce_samples
     * 次有效才退出。单次 glitch（例如开关继电器时的电源毛刺）不该让风扇抽一下。
     *
     * ⚠️ 去抖**只在本模块做一次**：状态机消费的是这里输出的、已经去过抖的
     *    `in_temp_fallback` / `tach_lost`。两处各去抖一遍会让「到底延迟了几拍」
     *    变得说不清楚，也难写测试。
     */
    uint16_t invalid_debounce_samples;
    uint16_t recovery_debounce_samples;

    /** 用户偏移，毫摄氏度，**加在设定温度上**：调大 = 目标温度更高 = 同温度下更慢。 */
    int32_t  temp_offset_milli_c;

    /** 有效占空比下限（%）。0 表示不设下限；典型值 20（4 线规范的有效区下沿）。 */
    uint8_t  duty_min_effective_percent;

    /** 实测转速的 EMA 系数 (0, 1]；0 或 1 表示不过滤。抑制 FG 量化噪声。 */
    float    rpm_ema_alpha;
} thermal_config_t;

/** 控制器运行状态。字段是内部实现，调用方通过 thermal_control_get_status() 读取。 */
typedef struct {
    thermal_config_t cfg;
    fan_curve_t      curve_state;      /**< 外环（含迟滞记忆） */
    pi_t             pi_state;         /**< 内环 */

    float            rpm_filtered;     /**< EMA 之后的实测转速 */
    bool             rpm_filtered_valid; /**< rpm_filtered 是否已有意义 */
    bool             pi_active;        /**< 上拍是否走的内环 PI（用于无扰切换） */
    uint32_t         target_rpm;       /**< 本周期的目标转速 */
    uint8_t          duty_percent;     /**< 本周期的输出占空比 */

    uint16_t         invalid_streak;   /**< 温度：连续「无效」的拍数 */
    uint16_t         valid_streak;     /**< 温度：连续「有效」的拍数（退出兜底用） */
    bool             in_temp_fallback; /**< 温度是否处于兜底状态 */
    bool             tach_lost;        /**< 转速是否已判定为丢失（去抖后） */
    uint16_t         tach_lost_streak; /**< 转速：与当前认定相反的连续拍数 */

    uint16_t         not_tracking_streak;  /**< 内环追不上目标的连续拍数 */
    fan_alarm_t      alarms;

    uint32_t         last_ms;          /**< 上次 step 的时刻，用于算真实 dt */
    bool             primed;           /**< last_ms 是否已有意义 */
    bool             ready;            /**< init 是否成功过 */
} thermal_controller_t;

/** 只读状态快照（遥测与显示用，避免外部直接触碰内部字段）。 */
typedef struct {
    uint32_t    target_rpm;
    float       rpm_filtered;
    uint8_t     duty_percent;
    int32_t     temp_offset_milli_c;
    uint32_t    target_temp_milli_c;  /**< 当前工作点对应的曲线温度（供显示 Tset） */
    fan_alarm_t alarms;
    bool        in_temp_fallback;
    bool        tach_lost;
} thermal_status_t;

/**
 * @brief 装载配置并复位。输出占空比初始化为 0。
 *
 * @return error_t ERR_OK 成功；
 *                 ERR_INVALID_PARAM 空指针、control_period_ms 为 0、
 *                                   duty_min_effective_percent > 100、
 *                                   或曲线/PI 的配置非法（透传它们的校验结果）。
 */
error_t thermal_control_init(thermal_controller_t *ctl, const thermal_config_t *cfg);

/**
 * @brief 推进一个控制周期。
 *
 * @param ctl        控制器实例。
 * @param temp       温度采样；传 NULL 视作「本拍无效」。
 * @param rpm        转速采样；传 NULL 视作「本拍无效」。
 * @param now_ms     当前 tick（bsp_system_get_tick_ms）。
 * @param duty_out   输出，期望占空比（0 或 [duty_min, 100]）。
 * @param alarms_out 输出，当前报警位图。
 *
 * @return error_t ERR_OK；ERR_INVALID_PARAM 空指针；ERR_NOT_INITIALIZED 未 init。
 *
 * @note dt 由**真实 tick 差**算出（首拍与 tick 未前进时退化为标称周期），
 *       并夹在 [标称周期, 10×标称周期] 内——后者用来挡住「主循环被长时间
 *       阻塞后，积分一拍吃进一个巨大的 dt」这种极端情况。
 */
error_t thermal_control_step(thermal_controller_t *ctl,
                             const temperature_sample_t *temp,
                             const fan_rpm_sample_t *rpm,
                             uint32_t now_ms,
                             uint8_t *duty_out,
                             fan_alarm_t *alarms_out);

/**
 * @brief 设置用户温度偏移（毫摄氏度，可正可负）。
 *
 * @return error_t ERR_OK；ERR_INVALID_PARAM ctl 为空。
 */
error_t thermal_control_set_temp_offset(thermal_controller_t *ctl, int32_t offset_milli_c);

/**
 * @brief 在线整定内环 PI 增益（串口 PID 命令）。
 *
 * @return error_t ERR_OK；ERR_INVALID_PARAM ctl 为空或增益为负。
 */
error_t thermal_control_set_gains(thermal_controller_t *ctl, float kp, float ki);

/**
 * @brief 取只读状态快照。
 *
 * @return error_t ERR_OK；ERR_INVALID_PARAM 空指针；ERR_NOT_INITIALIZED 未 init。
 */
error_t thermal_control_get_status(const thermal_controller_t *ctl, thermal_status_t *out);

#endif /* MODULE_THERMAL_CONTROL_H */
