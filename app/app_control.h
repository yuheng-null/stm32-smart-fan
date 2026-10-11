/**
 * @file app_control.h
 * @brief 控制编排：把采样、级联控制器、状态机、BSP 串成一条闭环。
 *
 * 这一层是**唯一**知道「温度传感器接在哪、风扇挂在哪、按键是哪两个」的地方，
 * 它负责把 BSP 的类型翻译成 module 层的值类型、再把 module 的输出翻译回 BSP 调用。
 * module/ 里没有一个函数认识 bsp_key_t 或 ssd1306_t——这就是它们能在 PC 上
 * 单测的原因。
 *
 * 依赖方向：APP → MODULE、BSP、INTERFACE（见 docs/architecture.md）。
 */
#ifndef APP_CONTROL_H
#define APP_CONTROL_H

#include <stdbool.h>
#include <stdint.h>

#include "interface/error.h"
#include "module/common/control_types.h"
#include "module/fan_fsm/fan_fsm.h"   /* fan_cmd_t：本层是命令的投递方 */

/** 供显示与遥测使用的一次状态快照。 */
typedef struct {
    fan_mode_t  mode;
    bool        temp_valid;
    int32_t     temp_milli_c;
    error_t     temp_status;
    uint16_t    adc_raw;
    bool        rpm_valid;
    uint32_t    rpm;
    uint8_t     duty_percent;
    uint32_t    target_rpm;
    int32_t     target_temp_milli_c;   /**< 曲线起步点 + 用户偏移，可正可负 */
    uint8_t     manual_duty_percent;
    fan_alarm_t alarms;

    /*
     * 去抖后的传感器可用性（来自控制器，不是单次采样的原始结论）。
     * 单独存一份，是为了让「串口命令要顺手推进一下状态机」这类路径不必
     * 从报警位的某个 bit 反推有效性——那种写法会在报警语义调整时静默失效。
     */
    bool        temp_ok;
    bool        tach_ok;
} app_status_t;

/**
 * @brief 初始化控制器与状态机（不碰硬件，硬件由 bsp 各自 init）。
 *
 * @param oled_present 屏幕是否可用；不可用会置上 OLED_ABSENT 报警（仅显示降级）。
 *
 * @return error_t ERR_OK；其余为 module 层的配置校验失败原因。
 */
error_t app_control_init(bool oled_present);

/**
 * @brief 控制节拍任务（100 ms）。
 *
 * 一次跑完：采样 → 级联控制 → 状态机 → 写硬件 → 更新快照。
 */
void app_control_task(void);

/**
 * @brief 把一条语义命令投递给状态机并立即生效。
 *
 * 供 app_console 调用。命令以 fan_cmd_t 表达，console 模块不认识它、
 * 状态机也不认识 console 的命令枚举，两者在 app 层对接。
 *
 * @param cmd   命令。
 * @param value 参数（占空比 / 温度偏移）。
 *
 * @return error_t ERR_OK；ERR_NOT_INITIALIZED 尚未 init。
 */
error_t app_control_dispatch_cmd(fan_cmd_t cmd, int32_t value);

/**
 * @brief 设定**绝对**目标温度（毫摄氏度）。
 *
 * 用户（串口 `TEMP 32.5`）想的是「风扇从 32.5 ℃ 开始转」，这是相对曲线起步点
 * 的一个偏移。换算放在本层——因为**曲线起步点只有这里知道**，
 * 让 console 层去猜会变成两份事实。
 *
 * @param milli_c 目标温度（毫摄氏度）。
 *
 * @return error_t ERR_OK；ERR_NOT_INITIALIZED 尚未 init。
 */
error_t app_control_set_target_temp(int32_t milli_c);

/**
 * @brief 取当前设定温度（毫摄氏度），供回显使用。
 *
 * @return int32_t 设定温度；未初始化时返回 0。
 */
int32_t app_control_get_target_temp(void);

/**
 * @brief 在线整定内环 PI。
 *
 * @return error_t ERR_OK；其余为参数校验失败原因。
 */
error_t app_control_set_gains(float kp, float ki);

/**
 * @brief 取最近一次控制节拍的状态快照。
 *
 * @return bool true 表示快照有效。
 */
bool app_control_get_status(app_status_t *out);

#endif /* APP_CONTROL_H */
