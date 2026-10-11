/**
 * @file ir_remote.c
 * @brief NEC 红外解码状态机的实现。
 *
 * 时序与输入约定见 ir_remote.h 顶部。实现上值得说明的有三处：
 *
 * ── 1. 判「是 0 还是 1」按**顺序试窗口**，不取中间阈值 ────────────────
 *   两个候选时长（0.56 ms 与 1.69 ms）之间留了空档，所以「先试 0 的窗口、
 *   再试 1 的窗口、都不中才拒」是精确的，不会因为取了个阈值而把边界上的
 *   帧误判。引导码的 space（4.5 ms）与重复码的 space（2.25 ms）同理。
 *
 * ── 2. 失步之后**重新按 IDLE 判定**，而不是干等下一帧 ─────────────────
 *   典型场景：按键中途松开，或线上掉了一个沿。此时状态机停在半路，
 *   下一个边沿未必符合它期待的形态。与其丢弃、等一整帧，不如就地按
 *   「这是不是一个引导 mark」重新判一次——本工程实测能少丢一帧。
 *
 * ── 3. 真正把关的是末尾的反码校验 ────────────────────────────────────
 *   前面那些时长窗口都开得比较宽（±30% 上下），因为遥控器的晶振、
 *   发射芯片、接收头都有偏差，窗口收紧会把好帧挡在外面。
 *   「这到底是不是一帧」由 `cmd` 与 `~cmd` 是否互为反码来定——
 *   32 位里错一位就有约 50% 的概率被它逮住，连错两位几乎必中。
 * ────────────────────────────────────────────────────────────────────
 */

#include "module/ir_remote/ir_remote.h"

#include <stddef.h>

/** @brief 值是否落在闭区间 [lo, hi] 内。 */
static bool in_window(uint32_t value, uint32_t lo, uint32_t hi)
{
    return (value >= lo) && (value <= hi);
}

/** @brief 记一次拒帧。 */
static void count_reject(ir_remote_t *ir, ir_reject_t reason)
{
    ir->stats.rejects++;
    ir->stats.last_reject = reason;
}

/**
 * @brief 把一个「刚结束的 mark」按引导码判一次。
 *
 * IDLE 与「失步后重判」都走这里：是引导 mark 就进 LEADER_SPACE，
 * 否则留在 IDLE 继续等。
 */
static void accept_as_idle(ir_remote_t *ir, uint32_t mark_us)
{
    if (in_window(mark_us, IR_LEADER_MARK_MIN_US, IR_LEADER_MARK_MAX_US)) {
        ir->state = IR_STATE_LEADER_SPACE;
    } else {
        ir->state = IR_STATE_IDLE;
    }
}

/** @brief 32 位收齐：校验反码，通过就吐一帧。 */
static bool finish_frame(ir_remote_t *ir, ir_frame_t *out)
{
    const uint32_t word = ir->shift;

    const uint8_t addr_lo = (uint8_t)(word & 0xFFU);
    const uint8_t addr_hi = (uint8_t)((word >> 8U) & 0xFFU);
    const uint8_t cmd     = (uint8_t)((word >> 16U) & 0xFFU);
    const uint8_t cmd_inv = (uint8_t)((word >> 24U) & 0xFFU);

    /* 命令字节的反码校验：两种地址格式都做，所以扩展 NEC 也不会误收。 */
    if (cmd_inv != (uint8_t)~cmd) {
        count_reject(ir, IR_REJECT_CHECKSUM);
        ir->state = IR_STATE_IDLE;
        return false;
    }

    /*
     * 地址：第 2 字节是第 1 字节的反码 → 标准 NEC，取 8 位；
     * 否则按扩展 NEC 取 16 位（低 8 位先发）。
     */
    const bool extended = (addr_hi != (uint8_t)~addr_lo);

    if (out != NULL) {
        out->address   = extended ? (uint16_t)((uint16_t)addr_lo | ((uint16_t)addr_hi << 8U))
                                  : (uint16_t)addr_lo;
        out->command   = cmd;
        out->extended  = extended;
        out->is_repeat = false;
    }

    ir->stats.frames++;
    ir->state = IR_STATE_IDLE;

    return true;
}

/* ----------------------------------------------------------------- 对外 */

error_t ir_remote_init(ir_remote_t *ir)
{
    if (ir == NULL) {
        return ERR_INVALID_PARAM;
    }

    *ir = (ir_remote_t){0};

    /* last_reject 的 0 是 IR_REJECT_NONE，正好表示「还没拒过」。
       其余字段清零即「等引导码、没在收位」。 */
    return ERR_OK;
}

void ir_remote_reset(ir_remote_t *ir)
{
    if (ir == NULL) {
        return;
    }

    ir->state = IR_STATE_IDLE;
    ir->bits  = 0U;
    ir->shift = 0U;
}

bool ir_remote_feed(ir_remote_t *ir, bool level_after, uint32_t interval_us, ir_frame_t *out)
{
    if (ir == NULL) {
        return false;
    }

    ir->stats.edges++;

    /*
     * 边沿之后的电平决定**刚过去的那一段**是什么：
     *   升到高（level_after == true）→ 刚结束的是低电平段 = mark（载波突发）；
     *   降到低（level_after == false）→ 刚结束的是高电平段 = space（间隔）。
     * 接收头 VS1838B 空闲输出高、收到 38 kHz 时拉低，所以是这个对应关系。
     */
    const bool ended_mark  = level_after;
    const bool ended_space = !level_after;

    bool decoded = false;

    switch (ir->state) {
    case IR_STATE_IDLE:
        /* 只认一个约 9 ms 的 mark，别的时长一概忽略——
           于是上电后的第一个边沿（时长无意义）自然被丢掉。 */
        if (ended_mark) {
            accept_as_idle(ir, interval_us);
        }
        break;

    case IR_STATE_LEADER_SPACE:
        if (ended_space) {
            /* 先试重复码（窗口更窄），再试数据帧的引导 space。两者不重叠。 */
            if (in_window(interval_us, IR_REPEAT_SPACE_MIN_US, IR_REPEAT_SPACE_MAX_US)) {
                if (out != NULL) {
                    out->address   = 0U;
                    out->command   = 0U;
                    out->extended  = false;
                    out->is_repeat = true;
                }
                ir->stats.repeats++;
                ir->state = IR_STATE_IDLE;
                decoded   = true;
            } else if (in_window(interval_us, IR_LEADER_SPACE_MIN_US, IR_LEADER_SPACE_MAX_US)) {
                ir->bits  = 0U;
                ir->shift = 0U;
                ir->state = IR_STATE_BIT_MARK;
            } else {
                count_reject(ir, IR_REJECT_LEADER_SPACE);
                ir->state = IR_STATE_IDLE;
            }
        } else {
            /* 该来 space 却来了 mark：边上多了一跳或丢了一沿。就地重判。 */
            accept_as_idle(ir, interval_us);
        }
        break;

    case IR_STATE_BIT_MARK:
        if (ended_mark) {
            if (in_window(interval_us, IR_BIT_MARK_MIN_US, IR_BIT_MARK_MAX_US)) {
                ir->state = IR_STATE_BIT_SPACE;
            } else {
                count_reject(ir, IR_REJECT_BIT_MARK);
                ir->state = IR_STATE_IDLE;
            }
        } else {
            /*
             * 该来 mark 却来了 space：边上丢了一沿。
             * ⚠️ 这里**不能**拿 interval_us 去按引导码判定——它是一个 **space**
             *    的时长，而此刻线上刚拉低、一个新的 mark 才刚开始。交给 IDLE，
             *    由**下一个**边沿（那个 mark 的结束沿）去判。
             */
            count_reject(ir, IR_REJECT_BIT_MARK);
            ir->state = IR_STATE_IDLE;
        }
        break;

    case IR_STATE_BIT_SPACE:
        if (ended_space) {
            uint32_t bit;

            if (in_window(interval_us, IR_BIT_SPACE0_MIN_US, IR_BIT_SPACE0_MAX_US)) {
                bit = 0U;
            } else if (in_window(interval_us, IR_BIT_SPACE1_MIN_US, IR_BIT_SPACE1_MAX_US)) {
                bit = 1U;
            } else {
                count_reject(ir, IR_REJECT_BIT_SPACE);
                ir->state = IR_STATE_IDLE;
                break;
            }

            /* NEC 低位先发：第 n 个收到的位就放在第 n 位。 */
            ir->shift |= (bit << ir->bits);
            ir->bits++;

            if (ir->bits >= IR_NEC_BIT_COUNT) {
                decoded = finish_frame(ir, out);
            } else {
                ir->state = IR_STATE_BIT_MARK;
            }
        } else {
            count_reject(ir, IR_REJECT_BIT_SPACE);
            accept_as_idle(ir, interval_us);
        }
        break;

    default:
        /* 枚举被写坏时才可能到这儿；回到 IDLE 而不是原地打转。 */
        ir->state = IR_STATE_IDLE;
        break;
    }

    return decoded;
}

error_t ir_remote_get_stats(const ir_remote_t *ir, ir_stats_t *out)
{
    if ((ir == NULL) || (out == NULL)) {
        return ERR_INVALID_PARAM;
    }

    *out = ir->stats;

    return ERR_OK;
}

const char *ir_reject_name(ir_reject_t reason)
{
    static const char *const k_names[IR_REJECT_COUNT] = {
        "none",         /* IR_REJECT_NONE */
        "leader-space", /* IR_REJECT_LEADER_SPACE */
        "bit-mark",     /* IR_REJECT_BIT_MARK */
        "bit-space",    /* IR_REJECT_BIT_SPACE */
        "checksum",     /* IR_REJECT_CHECKSUM */
    };

    if ((uint32_t)reason >= (uint32_t)IR_REJECT_COUNT) {
        return "?";
    }

    return k_names[(uint32_t)reason];
}
