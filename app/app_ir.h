/**
 * @file app_ir.h
 * @brief 红外遥控的编排：把收到的帧翻译成风扇动作。
 *
 * ── 这一层做什么 ────────────────────────────────────────────────────
 *   bsp/bsp_ir 把「边沿 + 时刻」交上来，module/ir_remote 把它解成帧，
 *   本层负责**决定一帧意味着什么**：
 *     · 正常模式：查键表，翻译成与按键/串口同一套的语义命令；
 *     · 学习模式：只回显收到的码，不动风扇（这样换遥控器时不会误触发）。
 *
 * ── 为什么解码不在中断里 ────────────────────────────────────────────
 *   见 bsp_ir.h：一帧 68 个边沿，在中断里做时序判定会长时间占着 CPU，
 *   而串口与 FG 捕获丢的是真数据。中断只入队，本层在主循环里从容解。
 *
 * ── 遥控器键表 ──────────────────────────────────────────────────────
 *   默认表只放了**常见 17 键 NEC 遥控器**（地址 0x00）里已核实的六个键，
 *   够控制这台风扇的全部功能。「+/-」两个键是**跟着模式走**的——与板上的
 *   K1/K2 完全一致（AUTO 下调目标温度，MANUAL 下调档位），所以不用记两套。
 *
 *   换遥控器 / 想绑别的键：开 `IR LEARN 1`，按下想用的键，串口会打出它的
 *   地址与命令码；把那一行照抄进 app_ir.c 的 s_bindings[] 即可。
 *   （键表用 0xFFFF 作地址通配，所以地址不同的同款遥控器也能直接用。）
 */
#ifndef APP_IR_H
#define APP_IR_H

#include <stdbool.h>
#include <stdint.h>

#include "interface/error.h"

/** 遥控接收的运行状况，供串口 `IR` 命令打印。 */
typedef struct {
    uint32_t address;    /**< 最近一帧的地址（标准 NEC 为 0..0xFF） */
    uint8_t  command;    /**< 最近一帧的命令字节 */
    bool     extended;   /**< 最近一帧是否扩展 NEC */
    bool     have_last;  /**< 是否已经收到过一帧 */
    uint32_t frames;     /**< 解出的数据帧数 */
    uint32_t repeats;    /**< 解出的重复帧数 */
    uint32_t rejects;    /**< 拒帧数 */
    const char *last_reject; /**< 最近一次拒帧的原因（短名） */
    uint32_t edges;      /**< 收到的边沿数（含被丢弃的） */
    uint32_t dropped;    /**< 缓冲满被丢掉的边沿数 */
    bool     learn;      /**< 是否处于学习模式 */
} app_ir_status_t;

/**
 * @brief 初始化遥控接收。
 *
 * @return error_t ERR_OK；透传 bsp_ir_init() 的错误码。
 *
 * @note 必须在 bsp_ir_init() 能成功的前提下调用（即 bsp_fan_init() 之后）。
 */
error_t app_ir_init(void);

/**
 * @brief 周期任务：把 BSP 缓冲里的边沿取完、解码、翻译成动作。
 *
 * 由调度器按控制节拍调用（100 ms）。一帧 NEC 是 68 个边沿、按住不放时
 * 每 ~110 ms 一帧，所以这个节奏与到达速率是匹配的。
 */
void app_ir_task(void);

/**
 * @brief 开关学习模式。
 *
 * 学习模式下只回显收到的码、不执行任何动作——换遥控器时不会打着风扇。
 *
 * @return error_t ERR_OK；ERR_INVALID_PARAM 未初始化。
 */
error_t app_ir_set_learn(bool on);

/** @brief 取运行状况快照。返回 false 表示未初始化。 */
bool app_ir_get_status(app_ir_status_t *out);

#endif /* APP_IR_H */
