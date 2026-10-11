/**
 * @file ir_remote.h
 * @brief 红外遥控接收：NEC 协议的解码状态机。
 *
 * 纯计算：输入是一串「边沿 + 距上一个边沿的时长」，输出是解出的帧（或一次拒绝）。
 * 不含 HAL、不含寄存器、不看引脚——所以整条解码链可以在 PC 上跑，
 * 用合成的波形做穷举测试（见 tests/test_ir_remote.c）。
 *
 * ── 输入约定（这一条必须记住，否则状态机会错位）──────────────────────
 *   每来一个边沿调用一次 ir_remote_feed()，告诉它两件事：
 *     · 边沿**之后**的电平；
 *     · 这个电平**之前**持续了多久（微秒）。
 *
 *   也就是说：`level_after == true`（升到高）时，这段时间是**高电平**的时长；
 *   `level_after == false` 时，是**低电平**的时长。
 *   本工程里接收头 VS1838B 的输出是**空闲高、收到 38 kHz 载波时拉低**，
 *   所以「低电平段 = 载波突发（mark）」「高电平段 = 间隔（space）」。
 *
 * ── 为什么第一个边沿一定是无效的 ─────────────────────────────────────
 *   上电后或长按间隙里，线上一直是高，没人知道它高了多久。所以第一个边沿
 *   带着一个**没有意义的时长**进来。状态机在 IDLE 里只认「一个约 9 ms 的
 *   mark」，别的时长一概忽略——于是这个无意义的首边沿自然被丢掉，
 *   不需要调用方特判。
 *
 * ── NEC 的时序（标称值）─────────────────────────────────────────────
 *
 *     引导码      mark 9.0 ms + space 4.5 ms
 *     重复码      mark 9.0 ms + space 2.25 ms（按住不放时每 ~110 ms 一帧）
 *     数据位      每 bit：mark 0.5625 ms + space（0 → 0.5625 ms，1 → 1.6875 ms）
 *     帧结构      addr, ~addr, cmd, ~cmd，共 32 位，**低位先发**
 *
 *   接收头内部已经把 38 kHz 载波解调掉了，输出端看到的是上面这些 mark/space
 *   的**包络**，这也是为什么用「边沿 + 时长」就能解码。
 *
 * ── 两种地址格式 ────────────────────────────────────────────────────
 *   · 标准 NEC：第 2 字节是第 1 字节的反码 → 8 位地址。
 *   · 扩展 NEC：第 2 字节不是反码 → 16 位地址（第 1 字节为低 8 位）。
 *   两者**命令字节的反码校验都做**，所以哪种格式都不会误收。
 *
 * ⚠️ 容差都写成「毫秒级常数」而不是标称值的百分数：遥控器与接收头的晶振
 *    都有偏差，芯片方案也不止一家。宁可窗口宽一点，把「是否真是一帧」交给
 *    末尾的反码校验去判——那一条才是真正的门。
 */
#ifndef MODULE_IR_REMOTE_H
#define MODULE_IR_REMOTE_H

#include <stdbool.h>
#include <stdint.h>

#include "interface/error.h"

/* --------------------------------------------------------------- 时序 */

/** NEC 的基本时间单位（微秒）。规范值是 562.5 µs，取整到 562。 */
#define IR_NEC_UNIT_US          (562U)

/**
 * 各段的接受窗口（微秒）。
 *
 * 窗口以「帧内不会互相重叠」为准，而不是死扣百分数：
 *   · 重复码的 space（2.25 ms）与引导码的 space（4.5 ms）之间留了空档，
 *     两边都不挨着，所以**按顺序试**就能唯一定位，不需要再取阈值；
 *   · bit0 的 space（0.5625 ms）与 bit1 的（1.6875 ms）同理。
 */
#define IR_LEADER_MARK_MIN_US   (6000U)     /* 标称 8992，-33% */
#define IR_LEADER_MARK_MAX_US   (12000U)    /*            +33% */
#define IR_LEADER_SPACE_MIN_US  (3000U)     /* 标称 4496，-33% */
#define IR_LEADER_SPACE_MAX_US  (6200U)     /*            +38% */
#define IR_REPEAT_SPACE_MIN_US  (1400U)     /* 标称 2248，-38% */
#define IR_REPEAT_SPACE_MAX_US  (2900U)     /*            +29% */
#define IR_BIT_MARK_MIN_US      (280U)      /* 标称 562， -50% */
#define IR_BIT_MARK_MAX_US      (1000U)     /*           +78% */
#define IR_BIT_SPACE0_MIN_US    (250U)      /* 标称 562 */
#define IR_BIT_SPACE0_MAX_US    (1050U)
#define IR_BIT_SPACE1_MIN_US    (1150U)     /* 标称 1687 */
#define IR_BIT_SPACE1_MAX_US    (2400U)

/** 一帧数据的位数：addr(~addr)、cmd(~cmd)。 */
#define IR_NEC_BIT_COUNT        (32U)

/* --------------------------------------------------------------- 类型 */

/** 解出来的一帧。 */
typedef struct {
    uint16_t address;   /**< 标准 NEC 为 0..0xFF；扩展 NEC 为 16 位 */
    uint8_t  command;   /**< 命令字节 */
    bool     extended;  /**< true = 扩展 NEC（第 2 字节不是第 1 字节的反码） */
    bool     is_repeat; /**< true = 重复帧（按住不放时发的那个），此帧无地址/命令 */
} ir_frame_t;

/** 一帧被拒的原因。单独记下来是为了让「遥控不灵」能查到具体哪一步。 */
typedef enum {
    IR_REJECT_NONE = 0,
    IR_REJECT_LEADER_SPACE,  /**< 引导 mark 之后的 space 既不像 4.5 ms 也不像 2.25 ms */
    IR_REJECT_BIT_MARK,      /**< 某一位的 mark 时长离谱（多半是丢了沿或串了码） */
    IR_REJECT_BIT_SPACE,     /**< 某一位的 space 既不像 0 也不像 1 */
    IR_REJECT_CHECKSUM,      /**< 32 位收齐了，但命令字节与它的反码对不上 */
    IR_REJECT_COUNT,
} ir_reject_t;

/** 统计。都只增不减，取出来给 STATUS / IR 命令看。 */
typedef struct {
    uint32_t    edges;       /**< 收到的边沿总数 */
    uint32_t    frames;      /**< 成功解出的数据帧数（不含重复帧） */
    uint32_t    repeats;     /**< 成功解出的重复帧数 */
    uint32_t    rejects;     /**< 被拒的帧数 */
    ir_reject_t last_reject; /**< 最近一次拒绝的原因 */
} ir_stats_t;

/** 解码器状态。字段是内部实现，调用方不要直接读写。 */
typedef struct {
    /** 当前处于哪个阶段。 */
    enum {
        IR_STATE_IDLE = 0,      /**< 等引导码的 mark */
        IR_STATE_LEADER_SPACE,  /**< 已收到引导 mark，等 space 定性 */
        IR_STATE_BIT_MARK,      /**< 等某一位的 mark */
        IR_STATE_BIT_SPACE,     /**< 等某一位的 space（决定了这一位是 0 还是 1） */
    } state;

    uint32_t bits;      /**< 已收到的数据位数（0..32） */
    uint32_t shift;     /**< 收位用的移位寄存器：第 n 个收到的位放在第 n 位（NEC 低位先发） */

    ir_stats_t  stats;
} ir_remote_t;

/* --------------------------------------------------------------- 接口 */

/**
 * @brief 装载并复位解码器。
 *
 * @return error_t ERR_OK；ERR_INVALID_PARAM 入参为空。
 */
error_t ir_remote_init(ir_remote_t *ir);

/**
 * @brief 复位状态机（统计量保留）。串口上手动清状态时用。
 */
void ir_remote_reset(ir_remote_t *ir);

/**
 * @brief 喂一个边沿，推进状态机。
 *
 * @param ir          解码器实例。
 * @param level_after 边沿**之后**的电平。
 * @param interval_us 这个电平**之前**持续的微秒数。
 * @param out         解出一帧时填入；可为 NULL（只想知道「有没有解出来」）。
 *
 * @return true 表示本次调用解出了一帧（可能是数据帧，也可能是重复帧，
 *              由 out->is_repeat 区分）；false 表示还在收、或者刚拒了一帧。
 *
 * @note 拒帧不返回错误码：红外是**尽力而为**的链路，一次收错不值得打断调用方，
 *       统计量与 out 里都不体现，但可以从 ir_remote_get_stats() 查到原因。
 */
bool ir_remote_feed(ir_remote_t *ir, bool level_after, uint32_t interval_us, ir_frame_t *out);

/**
 * @brief 取一份统计快照。
 *
 * @return error_t ERR_OK；ERR_INVALID_PARAM 入参为空。
 */
error_t ir_remote_get_stats(const ir_remote_t *ir, ir_stats_t *out);

/** @brief 取某个拒绝原因的短名（给串口用）。 */
const char *ir_reject_name(ir_reject_t reason);

#endif /* MODULE_IR_REMOTE_H */
