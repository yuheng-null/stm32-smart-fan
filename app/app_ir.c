/**
 * @file app_ir.c
 * @brief 红外遥控编排的实现。设计与键表说明见 app_ir.h 顶部。
 */

#include "app/app_ir.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app/app_console.h"
#include "app/app_control.h"

#include "bsp/bsp_ir.h"

#include "module/common/control_types.h"
#include "module/ir_remote/ir_remote.h"

/* ----------------------------------------------------------------- 配置 */

/** 拼一行文本用的缓冲。 */
#define APP_IR_LINE_SIZE        (128U)

/** 手动档每次加减的百分点。与按键的档位步进一致。 */
#define APP_IR_DUTY_STEP        (10)

/** AUTO 下每次加减的目标温度（毫摄氏度）。与按键的 0.5 ℃ 一致。 */
#define APP_IR_TEMP_STEP_MILLI_C (500)

/** 键表里「任意地址」的通配值。 */
#define APP_IR_ADDR_ANY         (0xFFFFU)

/* ----------------------------------------------------------------- 类型 */

/** 一个遥控键能做的事。 */
typedef enum {
    IR_ACT_NONE = 0,
    IR_ACT_MODE_OFF,
    IR_ACT_MODE_AUTO,
    IR_ACT_MODE_MANUAL,
    IR_ACT_STEP_UP,     /**< 跟着当前模式走：MANUAL 调档位，其余调目标温度 */
    IR_ACT_STEP_DOWN,
    IR_ACT_RESET,
} ir_action_t;

/** 一条键绑定。 */
typedef struct {
    uint8_t     command;        /**< NEC 命令字节 */
    uint16_t    address;        /**< APP_IR_ADDR_ANY = 不校验地址 */
    ir_action_t action;
    bool        allow_repeat;   /**< 按住不放（重复帧）时是否重放这个动作 */
} ir_binding_t;

/**
 * 默认键表：常见 17 键 NEC 遥控器（地址 0x00）。
 *
 * 只放**已核实的六个键**——够控制这台风扇的全部功能；其余按键（数字、
 * EQ、音量等）的编码出处不一致，与其猜一个不如留空，用 `IR LEARN 1`
 * 现场读出来再往上加。
 *
 * 地址一律用通配：同款遥控器的仿制品地址可能不是 0x00，通配能直接用；
 * 想锁死地址，把 0xFFFF 换成 `IR LEARN` 打出来的那个值即可。
 */
static const ir_binding_t s_bindings[] = {
    { 0x45U, APP_IR_ADDR_ANY, IR_ACT_MODE_OFF,    false },  /* CH-  → 停机档    */
    { 0x46U, APP_IR_ADDR_ANY, IR_ACT_MODE_AUTO,   false },  /* CH   → 温控      */
    { 0x47U, APP_IR_ADDR_ANY, IR_ACT_MODE_MANUAL, false },  /* CH+  → 手动      */
    { 0x44U, APP_IR_ADDR_ANY, IR_ACT_STEP_DOWN,   true  },  /* |<<  → 调低      */
    { 0x40U, APP_IR_ADDR_ANY, IR_ACT_STEP_UP,     true  },  /* >>|  → 调高      */
    { 0x43U, APP_IR_ADDR_ANY, IR_ACT_RESET,       false },  /* >||  → 清报警    */
};

#define APP_IR_BINDING_COUNT    (sizeof(s_bindings) / sizeof(s_bindings[0]))

/* --------------------------------------------------------------- 状态 */

static ir_remote_t s_decoder;
static bool        s_ready      = false;
static bool        s_learn      = false;

/** 上一个边沿的时刻，用来算「距上一个边沿多久」。 */
static uint32_t    s_prev_us    = 0U;

/** 最近解出的一帧（供 IR 命令回显）。 */
static ir_frame_t  s_last_frame;
static bool        s_have_last  = false;

/** 当前按住的那个键对应的动作；重复帧来临时重放它。 */
static ir_action_t s_repeat_action = IR_ACT_NONE;

/* ----------------------------------------------------------------- 内部 */

/** @brief 把动作落到风扇上。 */
static void ir_apply_action(ir_action_t action)
{
    app_status_t status;

    switch (action) {
    case IR_ACT_MODE_OFF:
        (void)app_control_dispatch_cmd(FAN_CMD_MODE_OFF, 0);
        break;

    case IR_ACT_MODE_AUTO:
        (void)app_control_dispatch_cmd(FAN_CMD_MODE_AUTO, 0);
        break;

    case IR_ACT_MODE_MANUAL:
        (void)app_control_dispatch_cmd(FAN_CMD_MODE_MANUAL, 0);
        break;

    case IR_ACT_RESET:
        (void)app_control_dispatch_cmd(FAN_CMD_RESET, 0);
        break;

    case IR_ACT_STEP_UP:
    case IR_ACT_STEP_DOWN: {
        /*
         * 「调高/调低」跟着当前模式走，与板上的 K1/K2 完全一致：
         *   MANUAL → 改手动档位；其余模式 → 改目标温度。
         * 这样遥控器上只需要两个方向键，而不是每个模式各两个。
         */
        const int32_t dir = (action == IR_ACT_STEP_UP) ? 1 : -1;

        if (!app_control_get_status(&status)) {
            break;
        }

        if (status.mode == FAN_MODE_MANUAL) {
            int32_t duty = (int32_t)status.manual_duty_percent + (dir * APP_IR_DUTY_STEP);

            /* 先夹进合法区间：超出范围会被状态机拒掉，等于按键没反应。
               1~19 那段由状态机自己抬到 20，这里不用管。 */
            if (duty < 0) {
                duty = 0;
            } else if (duty > 100) {
                duty = 100;
            }

            (void)app_control_dispatch_cmd(FAN_CMD_MANUAL_SET, duty);
        } else {
            (void)app_control_set_target_temp(app_control_get_target_temp()
                                              + (dir * APP_IR_TEMP_STEP_MILLI_C));
        }
        break;
    }

    case IR_ACT_NONE:
    default:
        break;
    }
}

/** @brief 查表；没绑定就返回 NULL。 */
static const ir_binding_t *ir_find_binding(const ir_frame_t *frame)
{
    for (size_t i = 0U; i < APP_IR_BINDING_COUNT; i++) {
        const ir_binding_t *b = &s_bindings[i];

        if (b->command != frame->command) {
            continue;
        }
        if ((b->address != APP_IR_ADDR_ANY) && (b->address != frame->address)) {
            continue;
        }

        return b;
    }

    return NULL;
}

/** @brief 学习模式：把收到的码打出来（带一行机器可读的，方便直接抄进键表）。 */
static void ir_report_learn(const ir_frame_t *frame)
{
    char line[APP_IR_LINE_SIZE];

    if (frame->is_repeat) {
        const int n = snprintf(line, sizeof(line), "@IR,REP\r\n");

        if (n > 0) {
            (void)app_console_send(line, (uint32_t)n);
        }
        return;
    }

    const int n = snprintf(line, sizeof(line), "@IR,DAT,%u,%u,%u\r\n",
                           (unsigned)frame->address,
                           (unsigned)frame->command,
                           (unsigned)(frame->extended ? 1U : 0U));

    if (n > 0) {
        (void)app_console_send(line, (uint32_t)n);
    }

    const int m = snprintf(line, sizeof(line),
                           "  遥控码: 地址 0x%04X  命令 0x%02X  (%sNEC)\r\n",
                           (unsigned)frame->address,
                           (unsigned)frame->command,
                           frame->extended ? "扩展" : "标准");

    if (m > 0) {
        (void)app_console_send(line, (uint32_t)m);
    }
}

/** @brief 正常模式：把一帧翻译成动作。 */
static void ir_handle_frame(const ir_frame_t *frame)
{
    if (frame->is_repeat) {
        /* 按住不放：只有「允许重复」的动作才重放（方向键、档位键）。 */
        if (s_repeat_action != IR_ACT_NONE) {
            ir_apply_action(s_repeat_action);
        }
        return;
    }

    const ir_binding_t *binding = ir_find_binding(frame);

    /* 换绑之前先清掉：上一帧如果是方向键，它的重复帧不该延续到这一帧之后。 */
    s_repeat_action = IR_ACT_NONE;

    if (binding == NULL) {
        return;     /* 没绑定的键：忽略。想看它是什么码就用 IR LEARN 1。 */
    }

    if (binding->allow_repeat) {
        s_repeat_action = binding->action;
    }

    ir_apply_action(binding->action);
}

/* ----------------------------------------------------------------- 对外 */

error_t app_ir_init(void)
{
    s_ready         = false;
    s_learn         = false;
    s_prev_us       = 0U;
    s_have_last     = false;
    s_repeat_action = IR_ACT_NONE;

    const error_t err = bsp_ir_init();
    if (err != ERR_OK) {
        return err;
    }

    const error_t derr = ir_remote_init(&s_decoder);
    if (derr != ERR_OK) {
        return derr;
    }

    s_ready = true;

    return ERR_OK;
}

void app_ir_task(void)
{
    if (!s_ready) {
        return;
    }

    bsp_ir_edge_t edge;
    ir_frame_t    frame;

    while (bsp_ir_take_edge(&edge)) {
        /*
         * 两个时刻相减用**无符号**：时基是 32 位、约 4295 秒回绕一次，
         * 无符号减法在回绕处依然给出正确的差值。
         * （上电后的第一个边沿会得到一个没有意义的差值——解码器在「等引导码」
         *   那一档里本来就会忽略它，不需要在这里特判。）
         */
        const uint32_t dt = edge.us - s_prev_us;
        s_prev_us = edge.us;

        if (!ir_remote_feed(&s_decoder, edge.level, dt, &frame)) {
            continue;
        }

        s_last_frame = frame;
        s_have_last  = true;

        if (s_learn) {
            ir_report_learn(&frame);
        } else {
            ir_handle_frame(&frame);
        }
    }
}

error_t app_ir_set_learn(bool on)
{
    if (!s_ready) {
        return ERR_NOT_INITIALIZED;
    }

    if (on != s_learn) {
        /*
         * 切换时把两边的残留都清掉：缓冲里可能还压着半帧的边沿，
         * 状态机也可能停在收位途中。不清的话，切换后的第一帧会被拼歪。
         */
        bsp_ir_flush();
        ir_remote_reset(&s_decoder);
        s_repeat_action = IR_ACT_NONE;
    }

    s_learn = on;

    return ERR_OK;
}

bool app_ir_get_status(app_ir_status_t *out)
{
    if ((out == NULL) || !s_ready) {
        return false;
    }

    ir_stats_t stats;

    if (ir_remote_get_stats(&s_decoder, &stats) != ERR_OK) {
        return false;
    }

    out->address     = s_last_frame.address;
    out->command     = s_last_frame.command;
    out->extended    = s_last_frame.extended;
    out->have_last   = s_have_last;
    out->frames      = stats.frames;
    out->repeats     = stats.repeats;
    out->rejects     = stats.rejects;
    out->last_reject = ir_reject_name(stats.last_reject);
    out->edges       = stats.edges;
    out->dropped     = bsp_ir_get_dropped();
    out->learn       = s_learn;

    return true;
}
