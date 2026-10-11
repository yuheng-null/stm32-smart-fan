/**
 * @file fan_fsm.h
 * @brief 风扇运行模式状态机（纯策略，不含数值算法）。
 *
 * 纯计算：输入是**值**（按键事件、传感器是否有效、控制器算出的自动占空比、
 * 当前 tick），输出也是**值**（模式、最终占空比、报警灯与蜂鸣器命令）。
 * 不碰 HAL、不 include thermal_control，因此可以在 PC 上喂任意事件序列做穷举测试。
 *
 * ── 为什么状态机不直接依赖控制器 ─────────────────────────────────────
 *   它只需要一个**整数**「自动模式下该给多少占空比」。让 app 先算好再喂进来，
 *   状态机就始终是个叶子模块——测试时不必构造控制器、不必模拟温度，
 *   直接给 `auto_duty = 42` 就能验证「AUTO 模式下输出是不是 42」。
 *
 * ── 传感器去抖在哪 ──────────────────────────────────────────────────
 *   在 thermal_control 里**只做一次**。本模块收到的 temp_valid / tach_valid
 *   是已经去过抖的结果，不再重复计数——两处各去抖一遍会让「到底延迟了几拍」
 *   说不清楚，也没法写确定的测试。
 *
 * ── 各模式的输出 ────────────────────────────────────────────────────
 *   INIT      占空比 0，报警灯灭 —— 上电到接管之间
 *   OFF       占空比 0，报警灯灭 —— ⚠️ 实测 0% 时风扇仍 ~295 rpm，是**怠速不是真停**
 *   MANUAL    手动档位，报警灯灭 —— 忽略传感器做控制，但仍如实上报报警
 *   AUTO      控制器算出的占空比，报警灯灭
 *   DEGRADED  控制器的安全兜底值，报警灯灭 —— 传感器失效但还能凑合运行
 *   FAULT     占空比 100%，报警灯亮、蜂鸣器响 —— 冷却系统失效安全 = 满速，需 RESET
 *
 *   报警灯亮在 FAULT 而不是过温：报警器件的语义是「这台冷却系统出问题了」，
 *   而不是「现在热」——热本身由转速去响应。冷却风机故障时应满速，绝不能断电。
 * ────────────────────────────────────────────────────────────────────
 */
#ifndef MODULE_FAN_FSM_H
#define MODULE_FAN_FSM_H

#include <stdbool.h>
#include <stdint.h>

#include "interface/error.h"
#include "module/common/control_types.h"

/** 逻辑按键（不引用 bsp_key 的 key_id_t，由 app 负责翻译）。 */
typedef enum {
    FAN_KEY_1 = 0,
    FAN_KEY_2,
    FAN_KEY_COUNT,
} fan_key_t;

/** 事件类型。 */
typedef enum {
    FAN_EVENT_NONE = 0,         /**< 无事发生，仅推进时间（长按计时用） */
    FAN_EVENT_KEY_PRESS,        /**< key 有效 */
    FAN_EVENT_KEY_RELEASE,      /**< key 有效 */
    FAN_EVENT_SENSOR_UPDATE,    /**< 传感器有效性有变化，触发升降级判定 */
    FAN_EVENT_CMD,              /**< 来自串口的模式/参数命令，cmd 与 value 有效 */
} fan_event_type_t;

/** 语义化命令（不引用 module/console 的类型，两边解耦）。 */
typedef enum {
    FAN_CMD_NONE = 0,
    FAN_CMD_MODE_OFF,
    FAN_CMD_MODE_AUTO,
    FAN_CMD_MODE_MANUAL,
    FAN_CMD_MANUAL_SET,         /**< value = 占空比 0..100 */
    FAN_CMD_TEMP_OFFSET_SET,    /**< value = 温度偏移（毫摄氏度）；绝对温度由 app 换算 */
    FAN_CMD_RESET,              /**< 清报警并回到 AUTO */
} fan_cmd_t;

/** 一次事件。用扁平结构而不是带指针的联合体：无动态内存，也便于逐帧记录回放。 */
typedef struct {
    fan_event_type_t type;
    uint32_t         tick_ms;   /**< 事件发生时刻 */
    fan_key_t        key;       /**< FAN_EVENT_KEY_* 时有效 */
    fan_cmd_t        cmd;       /**< FAN_EVENT_CMD 时有效 */
    int32_t          value;     /**< 命令参数 */
} fan_event_t;

/** 状态机配置。 */
typedef struct {
    uint32_t long_press_ms;         /**< 长按判定阈值（如 800 ms） */
    uint32_t min_dwell_ms;          /**< 模式最小驻留（如 500 ms），防连按导致模式闪烁 */
    int32_t  temp_step_milli_c;     /**< AUTO 下 K1/K2 的温度步进（如 500 = 0.5 ℃） */
    int32_t  temp_min_milli_c;      /**< 偏移下限 */
    int32_t  temp_max_milli_c;      /**< 偏移上限 */
    uint8_t  manual_min_percent;    /**< 手动档非零时的下限（20），避开 4 线规范未定义区 */
} fan_fsm_config_t;

/** 状态机状态。字段是内部实现，调用方不要直接读写。 */
typedef struct {
    fan_fsm_config_t cfg;

    fan_mode_t       mode;
    uint32_t         mode_entered_ms;

    uint8_t          manual_duty;          /**< 手动档位（0 或 [manual_min, 100]） */
    int32_t          temp_offset_milli_c;  /**< 用户温度偏移，透传给控制器 */
    bool             muted;                /**< 蜂鸣器静音（K2 长按切换） */

    bool             key_pressed[FAN_KEY_COUNT];
    uint32_t         key_press_tick[FAN_KEY_COUNT];
    bool             key_long_fired[FAN_KEY_COUNT];

    fan_alarm_t      alarms;               /**< 本模块自己判定出的报警位 */
    fan_alarm_t      held_alarms;          /**< 由 app 透传进来的外部报警（如未标定、屏不在） */
    fan_alarm_t      prev_total;           /**< 上一拍的报警全量，用于算出「新增」 */

    bool             ready;
} fan_fsm_t;

/** 状态机输出——app 依据它去调 bsp。 */
typedef struct {
    fan_mode_t   mode;
    uint8_t      duty_percent;         /**< 最终占空比命令 */
    bool         alarm_lamp_on;        /**< 继电器（报警灯） */
    bool         buzzer_on;
    fan_alarm_t  alarms;               /**< 当前全部报警位（含透传的外部报警） */
    fan_alarm_t  new_alarms;           /**< 本拍新出现的报警位 */
    int32_t      temp_offset_milli_c;  /**< 供 app 转交给控制器 */
    uint8_t      manual_duty_percent;
} fan_fsm_output_t;

/**
 * @brief 初始化到 FAN_MODE_INIT。
 *
 * @return error_t ERR_OK；ERR_INVALID_PARAM 空指针、
 *                           或 temp_min > temp_max 这类自相矛盾的配置。
 */
error_t fan_fsm_init(fan_fsm_t *fsm, const fan_fsm_config_t *cfg);

/**
 * @brief 推进状态机。
 *
 * @param fsm        实例。
 * @param event      本拍事件；**可以传 NULL**（等同 FAN_EVENT_NONE）。
 *                   长按判定依赖「每隔一拍都被调用一次」，所以即便没有事件，
 *                   app 也必须按控制节拍持续调用本函数。
 * @param now_ms     当前 tick。
 * @param auto_duty  控制器算出的占空比（AUTO/DEGRADED 采用，其它模式忽略）。
 * @param temp_valid 温度是否有效（**已去抖**）。
 * @param tach_valid 转速是否有效（**已去抖**）。
 * @param out        输出。
 *
 * @return error_t ERR_OK；ERR_INVALID_PARAM 空指针；ERR_NOT_INITIALIZED 未 init。
 */
error_t fan_fsm_step(fan_fsm_t *fsm, const fan_event_t *event, uint32_t now_ms,
                     uint8_t auto_duty, bool temp_valid, bool tach_valid,
                     fan_fsm_output_t *out);

/**
 * @brief 由 app 注入「外部报警位」（例如 NTC 未标定、OLED 不在）。
 *
 * 这些报警不影响模式迁移，但要能出现在遥测与屏幕上，所以让 app 透传进来，
 * 由本模块统一并入输出，避免 app 和状态机各拼一份、拼法不一致。
 *
 * @return error_t ERR_OK；ERR_INVALID_PARAM fsm 为空。
 */
error_t fan_fsm_set_external_alarms(fan_fsm_t *fsm, fan_alarm_t alarms);

/**
 * @brief 取当前模式。
 *
 * @return fan_mode_t；fsm 为空时返回 FAN_MODE_INIT。
 */
fan_mode_t fan_fsm_get_mode(const fan_fsm_t *fsm);

#endif /* MODULE_FAN_FSM_H */
