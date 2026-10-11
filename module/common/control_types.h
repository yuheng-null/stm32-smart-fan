/**
 * @file control_types.h
 * @brief 业务层共享的纯数据类型：采样快照、运行模式、报警位。
 *
 * 这个头文件是**只有类型、没有代码**的：任何 module 都可以安全地 include 它，
 * 不会因此引入依赖、更不会成环。
 *
 * ── 为什么单独放在 module/ 而不是 interface/ ──────────────────────────
 *   interface/ 放的是**硬件契约**（uart_if / i2c_if）——它们描述「怎么和硬件说话」。
 *   这里放的是**业务域概念**（一次温度采样、一个运行模式）——它们与硬件无关，
 *   换芯片也不会变。混进 interface/ 会让「契约层」的含义被稀释。
 *
 * ── 为什么值得单独一个文件 ───────────────────────────────────────────
 *   thermal_control / fan_fsm / telemetry / display_view 都需要「一次采样」
 *   这个概念。各写各的会让结构体字段慢慢漂移（一个加了 adc_raw，另一个忘了），
 *   而这种漂移不会报错、只会让上报的数字对不上号。
 * ────────────────────────────────────────────────────────────────────
 */
#ifndef MODULE_COMMON_CONTROL_TYPES_H
#define MODULE_COMMON_CONTROL_TYPES_H

#include <stdbool.h>
#include <stdint.h>

#include "interface/error.h"

/**
 * @brief 一次温度采样。status 沿用 temperature_from_adc_raw() 的返回码。
 *
 * valid 与 status 是冗余的，这样是有意的：status 保留「为什么无效」
 * （UNDERFLOW 是短路/极热，OVERFLOW 是断路/极冷），valid 只回答「能不能用」。
 * 调用方大多数时候只关心后者，只有在报警/降级决策时才需要前者。
 */
typedef struct {
    int32_t  milli_c;   /**< 摄氏毫度；**仅 valid 时有意义** */
    uint16_t adc_raw;   /**< 产生本次读数的 ADC 原始计数（不受未标定影响的量） */
    error_t  status;    /**< ERR_OK / ERR_UNDERFLOW / ERR_OVERFLOW / … */
    bool     valid;     /**< 恒等于 (status == ERR_OK)，由生产方填写 */
    uint32_t tick_ms;   /**< 采样时刻（bsp_system_get_tick_ms 的值） */
} temperature_sample_t;

/**
 * @brief 一次转速采样。status 是「取周期」与「周期→转速」两段的合并返回码。
 *
 * 失败原因必须保留，不能一律压成「转速为 0」：
 *   · ERR_NOT_READY   —— 只捕获到一个沿，还没凑出间隔（刚上电时正常现象）；
 *   · ERR_TIMEOUT     —— 1 s 没新沿（风扇停了，或 FG 线断了）；
 *   · ERR_OVERFLOW    —— 算出的转速超上限（FG 线上的毛刺，不是真转速）；
 *   · ERR_UNDERFLOW   —— 间隔长到不足 1 RPM。
 * 「转速 0」这一个数字分不出这四种情况，而上层对它们的处置并不一样。
 */
typedef struct {
    uint32_t rpm;       /**< 转速（观测单位）；**仅 valid 时有意义** */
    error_t  status;    /**< 见上 */
    bool     valid;     /**< 恒等于 (status == ERR_OK) */
    uint32_t tick_ms;   /**< 采样时刻 */
} fan_rpm_sample_t;

/** 运行模式——状态机在图里流转的就是它。 */
typedef enum {
    FAN_MODE_INIT = 0,  /**< 上电初始化，尚未接管输出 */
    FAN_MODE_OFF,       /**< 停机档：占空比 0。⚠️ 实测 0% 时风扇仍 ~295 rpm，不是真停 */
    FAN_MODE_MANUAL,    /**< 手动固定占空比 */
    FAN_MODE_AUTO,      /**< 温控级联（曲线 + 内环 PI） */
    FAN_MODE_DEGRADED,  /**< 传感器降级兜底：仍运行，但按安全策略而非正常策略 */
    FAN_MODE_FAULT,     /**< 不可恢复，需显式 RESET */
    FAN_MODE_COUNT,
} fan_mode_t;

/**
 * @brief 报警位图。
 *
 * 用 uint32_t 位图而不是 enum 类型：这些值天然要按位或、要整体上报一个整数。
 * C 的 enum 做位运算在部分编译器上会触发「枚举算术」告警，位图用整数类型
 * 才是本意。常量放匿名 enum，编译期求值、不占存储。
 */
typedef uint32_t fan_alarm_t;

enum {
    FAN_ALARM_NONE              = 0u,        /**< 无报警 */
    FAN_ALARM_TEMP_INVALID      = 1u << 0,   /**< 温度读数无效（超出有效量程） */
    FAN_ALARM_TACH_LOST         = 1u << 1,   /**< FG 无信号 / 停转 */
    FAN_ALARM_RPM_NOT_TRACKING  = 1u << 2,   /**< 内环长时间饱和仍追不上目标转速 */
    FAN_ALARM_TEMP_UNCALIBRATED = 1u << 3,   /**< NTC 参数未标定，曲线横轴不可信 */
    FAN_ALARM_OLED_ABSENT       = 1u << 4,   /**< 屏不在：仅显示降级，不影响控制 */
};

#endif /* MODULE_COMMON_CONTROL_TYPES_H */
