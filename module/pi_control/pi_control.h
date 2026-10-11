/**
 * @file pi_control.h
 * @brief 通用 PI 控制器（纯计算，与风扇无关，可复用）。
 *
 * 本文件不含 HAL、不含寄存器、不碰任何硬件。输入是「目标值 / 实测值 / 时间步长」，
 * 输出是一个控制量，因此可以在 PC 上直接仿真验证（见 tests/test_pi_control.c）。
 *
 * ── 为什么是 PI 而不是 PID ───────────────────────────────────────────
 *   依据 Microchip **AN3530《PID Fan Speed Control for 4-Wire Fans》**：
 *   风扇是单向驱动的一阶对象（加速靠电机、减速靠惯性与风阻），微分项对这类
 *   对象没有实际收益；更要紧的是，本工程转速由 **FG 捕获的周期**换算而来，
 *   低速时周期只有几十微秒、量化台阶明显，D 项会把这点量化噪声放大成控制量的
 *   抖动（风扇会听见「滋滋」的调节声）。
 *   所以这里**有意**不做 D 项，而不是漏了。将来若真要用，请在调用处对测量值
 *   单独做低通之后再喂进来。
 *
 * ── 抗积分饱和的做法 ─────────────────────────────────────────────────
 *   积分饱和是 PI 最经典的事故：输出被限幅顶住时，误差仍在把积分器往上推，
 *   等目标终于降下来，积分器要花很久才退回去——表现为「该降的时候降不下来」。
 *   本实现用**条件积分**（conditional integration，默认）：
 *
 *       输出已经顶在上/下限，且误差还在把它往同一个方向推 → 这一拍**不累加积分**
 *
 *   另加一道保险：积分器本身也夹在 [out_min, out_max] 内，即使条件积分
 *   偶有穿透，也不会跑到无穷。
 * ────────────────────────────────────────────────────────────────────
 */
#ifndef MODULE_PI_CONTROL_H
#define MODULE_PI_CONTROL_H

#include <stdbool.h>

#include "interface/error.h"

/**
 * @brief PI 配置。
 *
 * 增益的单位由调用者定义，但必须自洽：本工程里误差是「转速（观测单位）」，
 * 输出是「占空比（%）」，于是 kp 的单位是 %/rpm、ki 是 %/(rpm·s)。
 */
typedef struct {
    float kp;                       /**< 比例增益，≥ 0 */
    float ki;                       /**< 积分增益，≥ 0。0 = 纯比例 */
    float out_min;                  /**< 输出下限 */
    float out_max;                  /**< 输出上限，必须 > out_min */
    float out_rate_limit_per_s;     /**< 输出变化率上限（单位/秒）；**≤ 0 表示不限** */
    bool  allow_integrate_in_sat;   /**< true = 饱和时仍累加积分（不推荐，仅供对照测试） */
} pi_config_t;

/** PI 运行状态。字段是内部实现，调用方不要直接读写。 */
typedef struct {
    pi_config_t cfg;
    float       integrator;   /**< 积分累加器，**直接以输出单位存储**（不是误差单位） */
    float       last_out;     /**< 上次输出，速率限制的参考点 */
    bool        primed;       /**< last_out 是否已有意义 */
} pi_t;

/**
 * @brief 装载配置并复位。
 *
 * @return error_t ERR_OK 成功；
 *                 ERR_INVALID_PARAM pi/cfg 为空、kp 或 ki 为负、
 *                                   或 not(out_min < out_max)。
 */
error_t pi_init(pi_t *pi, const pi_config_t *cfg);

/**
 * @brief 无扰复位：把控制器预置到「当前正在输出的这个值」上。
 *
 * 用在**切模式**的时刻（例如从手动切进自动）：此时执行器已经停在某个值上，
 * 若把积分器清零，输出会从 0 猛冲一下。预置之后，只要误差为 0，第一拍的输出
 * 就等于这个值，扇叶不会抽一下。
 *
 * @param pi     实例。
 * @param output 当前实际输出（应落在 [out_min, out_max] 内，越界会被夹取）。
 *
 * @return error_t ERR_OK；ERR_INVALID_PARAM pi 为空。
 */
error_t pi_reset(pi_t *pi, float output);

/**
 * @brief 在线整定：只改增益，**不动积分器与上次输出**。
 *
 * 逐帧调参（串口 PID 命令）最忌讳的就是改个增益顺手把状态清了——
 * 那会让每一次调参都变成一次重启，永远看不出连续趋势。
 *
 * @return error_t ERR_OK；ERR_INVALID_PARAM pi 为空或增益为负。
 */
error_t pi_set_gains(pi_t *pi, float kp, float ki);

/**
 * @brief 推进一个控制周期（目标值 / 实测值形式）。
 *
 * @param pi          实例。
 * @param setpoint    目标值。
 * @param measurement 实测值。
 * @param dt_s        本周期**实际**时长（秒），必须 > 0。
 *                    由调用者用真实 tick 差算出，不要用名义周期——
 *                    否则偶发的调度抖动会让积分量悄悄偏掉。
 * @param out         输出，已夹到 [out_min, out_max] 并做过速率限制。
 *
 * @return error_t ERR_OK；ERR_INVALID_PARAM 空指针或 dt_s ≤ 0。
 */
error_t pi_step(pi_t *pi, float setpoint, float measurement, float dt_s, float *out);

/**
 * @brief 同 pi_step，但直接给误差（= setpoint − measurement）。
 *
 * 单独提供它有两个用处：单元测试可以脱离「目标/实测」直接喂误差；
 * 将来若要复用成位置环、流量环等别的内环，也不必先凑一对假的目标与实测。
 *
 * @return error_t ERR_OK；ERR_INVALID_PARAM 空指针或 dt_s ≤ 0。
 */
error_t pi_step_error(pi_t *pi, float error, float dt_s, float *out);

/**
 * @brief 取积分器现值（调试与遥测用）。
 *
 * @return float 积分器；pi 为空时返回 0。
 */
float pi_get_integrator(const pi_t *pi);

#endif /* MODULE_PI_CONTROL_H */
