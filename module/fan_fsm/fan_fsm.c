/**
 * @file fan_fsm.c
 * @brief 风扇运行模式状态机的实现。
 *
 * 模式语义与设计理由见 fan_fsm.h 顶部。实现上有三个地方值得说明：
 *
 * ── 1. 长按判定不依赖「有一个长按事件」────────────────────────────────
 *   按键驱动只给「按下 / 松开」两种事件，没有「长按」事件。本模块在**每一拍**
 *   都检查一次「还按着、且已超过阈值」，因此 app 必须按控制节拍持续调用本函数
 *   （即便这拍没有事件）。这样不需要额外定时器，也不会漏判。
 *
 * ── 2. 安全相关的迁移绕过「最小驻留」────────────────────────────────
 *   最小驻留是给用户连按做防抖的；降级与 FAULT 是安全动作，晚半秒都不行，
 *   所以它们用 ensure_mode() 立即生效，只有 AUTO↔MANUAL↔OFF 这类**用户切换**
 *   才走 try_enter_mode() 的驻留检查。
 *
 * ── 3. 「进入手动档」要播种一个起步占空比 ────────────────────────────
 *   若手动档从 0 开始，用户一切进手动风扇就停了，像是故障。所以进入手动档时
 *   用当前的自动值做种子，再按手动档的合法区间夹一下。
 * ────────────────────────────────────────────────────────────────────
 */

#include "module/fan_fsm/fan_fsm.h"

#include <stddef.h>

#include "module/fan_control/fan_control.h"

/** FAULT 下的占空比：冷却系统失效安全 = 满速。 */
#define FAN_FSM_FULL_DUTY_PERCENT   (100U)

/* ----------------------------------------------------------------- 内部 */

/**
 * @brief 无条件切换模式（用于安全动作与上电接管），刷新驻留计时。
 *
 * 只在目标模式与当前不同时才动——否则每拍都会把 mode_entered_ms 刷新成 now，
 * 最小驻留就永远满足不了，用户连按防抖会失效。
 */
static void ensure_mode(fan_fsm_t *fsm, fan_mode_t mode, uint32_t now_ms)
{
    if (fsm->mode != mode) {
        fsm->mode            = mode;
        fsm->mode_entered_ms = now_ms;
    }
}

/**
 * @brief 尝试切换模式（用于用户操作），受最小驻留约束。
 *
 * @return bool true 表示切换成功（或本来就在该模式）。
 */
static bool try_enter_mode(fan_fsm_t *fsm, fan_mode_t mode, uint32_t now_ms)
{
    if (fsm->mode == mode) {
        return true;
    }

    /* 无符号减法：tick 回绕安全。 */
    if ((uint32_t)(now_ms - fsm->mode_entered_ms) < fsm->cfg.min_dwell_ms) {
        return false;
    }

    fsm->mode            = mode;
    fsm->mode_entered_ms = now_ms;

    return true;
}

/** @brief 手动档的合法档位：0（停）或 [manual_min, 100]，中间那段不落。 */
static uint8_t sanitize_manual_duty(const fan_fsm_t *fsm, uint8_t duty)
{
    if ((duty > 0U) && (duty < fsm->cfg.manual_min_percent)) {
        return fsm->cfg.manual_min_percent;
    }
    return duty;
}

/** @brief 切进手动档时，用当前自动值做起步占空比。 */
static void seed_manual_duty(fan_fsm_t *fsm, uint8_t auto_duty)
{
    fsm->manual_duty = sanitize_manual_duty(fsm, auto_duty);
}

/** @brief 用户模式下按 K1 长按的循环顺序：AUTO → MANUAL → OFF → AUTO。 */
static fan_mode_t next_user_mode(fan_mode_t mode)
{
    switch (mode) {
    case FAN_MODE_AUTO:   return FAN_MODE_MANUAL;
    case FAN_MODE_MANUAL: return FAN_MODE_OFF;
    case FAN_MODE_OFF:    return FAN_MODE_AUTO;
    default:              return FAN_MODE_AUTO;   /* INIT/DEGRADED/FAULT 一律回 AUTO */
    }
}

static void handle_key_short(fan_fsm_t *fsm, fan_key_t key)
{
    switch (fsm->mode) {
    case FAN_MODE_AUTO: {
        /* 调**目标温度**：K1 调高（更暖、风扇更慢），K2 调低。 */
        const int32_t step = (key == FAN_KEY_1) ? fsm->cfg.temp_step_milli_c
                                                : -fsm->cfg.temp_step_milli_c;
        int32_t       next = fsm->temp_offset_milli_c + step;

        if (next < fsm->cfg.temp_min_milli_c) {
            next = fsm->cfg.temp_min_milli_c;
        }
        if (next > fsm->cfg.temp_max_milli_c) {
            next = fsm->cfg.temp_max_milli_c;
        }
        fsm->temp_offset_milli_c = next;
        break;
    }

    case FAN_MODE_MANUAL: {
        /*
         * 档位步进要**带方向**地避开 (0, manual_min) 那一段。
         * 只做「越界就抬到下限」是不够的：从 20% 往下走时，步进结果是 10%，
         * 若一律抬到 20% 就永远减不下去，K2 形同虚设。
         * 这里的规矩是「往上就抬到下限，往下就落到 0」。
         */
        const fan_duty_dir_t dir  = (key == FAN_KEY_1) ? FAN_DUTY_UP : FAN_DUTY_DOWN;
        uint8_t              next = fan_control_duty_step(fsm->manual_duty, dir);

        if ((next > 0U) && (next < fsm->cfg.manual_min_percent)) {
            next = (dir == FAN_DUTY_UP) ? fsm->cfg.manual_min_percent : 0U;
        }
        fsm->manual_duty = next;
        break;
    }

    default:
        /* OFF 没有可调的档位；DEGRADED / FAULT 不接受用户操作 */
        break;
    }
}

static void handle_key_long(fan_fsm_t *fsm, fan_key_t key, uint32_t now_ms, uint8_t auto_duty)
{
    if (fsm->mode == FAN_MODE_FAULT) {
        /* FAULT 下唯一的出口是 K2 长按复位。K1 一律忽略——
           「按几下就退出故障」不是一个安全的设计。 */
        if (key == FAN_KEY_2) {
            fsm->muted  = false;
            fsm->alarms = FAN_ALARM_NONE;
            ensure_mode(fsm, FAN_MODE_AUTO, now_ms);
        }
        return;
    }

    if (key == FAN_KEY_1) {
        const fan_mode_t target = next_user_mode(fsm->mode);

        if (try_enter_mode(fsm, target, now_ms) && (target == FAN_MODE_MANUAL)) {
            seed_manual_duty(fsm, auto_duty);
        }
    } else {
        /* K2 长按：日常用于静音蜂鸣器；FAULT 下则是复位（上面已处理）。 */
        fsm->muted = !fsm->muted;
    }
}

static void handle_command(fan_fsm_t *fsm, fan_cmd_t cmd, int32_t value,
                           uint32_t now_ms, uint8_t auto_duty)
{
    /* FAULT 粘住：除复位外的命令一律忽略。 */
    if ((fsm->mode == FAN_MODE_FAULT) && (cmd != FAN_CMD_RESET)) {
        return;
    }

    switch (cmd) {
    case FAN_CMD_MODE_OFF:
        (void)try_enter_mode(fsm, FAN_MODE_OFF, now_ms);
        break;

    case FAN_CMD_MODE_AUTO:
        (void)try_enter_mode(fsm, FAN_MODE_AUTO, now_ms);
        break;

    case FAN_CMD_MODE_MANUAL:
        if (try_enter_mode(fsm, FAN_MODE_MANUAL, now_ms)) {
            seed_manual_duty(fsm, auto_duty);
        }
        break;

    case FAN_CMD_MANUAL_SET: {
        int32_t d = value;

        if (d < 0) {
            d = 0;
        } else if (d > 100) {
            d = 100;
        }
        fsm->manual_duty = sanitize_manual_duty(fsm, (uint8_t)d);
        break;
    }

    case FAN_CMD_TEMP_OFFSET_SET: {
        int32_t v = value;

        if (v < fsm->cfg.temp_min_milli_c) {
            v = fsm->cfg.temp_min_milli_c;
        } else if (v > fsm->cfg.temp_max_milli_c) {
            v = fsm->cfg.temp_max_milli_c;
        }
        fsm->temp_offset_milli_c = v;
        break;
    }

    case FAN_CMD_RESET:
        fsm->muted  = false;
        fsm->alarms = FAN_ALARM_NONE;
        ensure_mode(fsm, FAN_MODE_AUTO, now_ms);   /* 复位是安全动作，立即生效 */
        break;

    default:
        break;
    }
}

/**
 * @brief 根据传感器有效性更新报警位与降级状态。
 *
 * FAULT 是**粘性**的：一旦进入就只认 RESET，不会因为传感器「某一拍看着还行」
 * 就自己跳出去。冷却系统反复在正常与故障之间抖动，比稳定停在故障态更危险。
 */
static void update_degradation(fan_fsm_t *fsm, bool temp_valid, bool tach_valid, uint32_t now_ms)
{
    fan_alarm_t alarms = fsm->alarms
                       & ~(fan_alarm_t)(FAN_ALARM_TEMP_INVALID | FAN_ALARM_TACH_LOST);

    if (!temp_valid) {
        alarms |= FAN_ALARM_TEMP_INVALID;
    }
    if (!tach_valid) {
        alarms |= FAN_ALARM_TACH_LOST;
    }
    fsm->alarms = alarms;

    if ((fsm->mode == FAN_MODE_FAULT) || (fsm->mode == FAN_MODE_INIT)) {
        return;
    }

    /* OFF / MANUAL：用户已经接管了输出，传感器失效只报警、不改策略。 */
    if ((fsm->mode != FAN_MODE_AUTO) && (fsm->mode != FAN_MODE_DEGRADED)) {
        return;
    }

    if (!temp_valid && !tach_valid) {
        ensure_mode(fsm, FAN_MODE_FAULT, now_ms);
    } else if (!temp_valid || !tach_valid) {
        ensure_mode(fsm, FAN_MODE_DEGRADED, now_ms);
    } else {
        ensure_mode(fsm, FAN_MODE_AUTO, now_ms);
    }
}

/* ----------------------------------------------------------------- 对外 */

error_t fan_fsm_init(fan_fsm_t *fsm, const fan_fsm_config_t *cfg)
{
    if ((fsm == NULL) || (cfg == NULL)) {
        return ERR_INVALID_PARAM;
    }
    if (cfg->temp_min_milli_c > cfg->temp_max_milli_c) {
        return ERR_INVALID_PARAM;
    }
    if (cfg->manual_min_percent > 100U) {
        return ERR_INVALID_PARAM;
    }

    *fsm = (fan_fsm_t){0};

    fsm->cfg = *cfg;
    fsm->mode = FAN_MODE_INIT;
    fsm->ready = true;

    return ERR_OK;
}

error_t fan_fsm_step(fan_fsm_t *fsm, const fan_event_t *event, uint32_t now_ms,
                     uint8_t auto_duty, bool temp_valid, bool tach_valid,
                     fan_fsm_output_t *out)
{
    if ((fsm == NULL) || (out == NULL)) {
        return ERR_INVALID_PARAM;
    }
    if (!fsm->ready) {
        return ERR_NOT_INITIALIZED;
    }

    /* 上电后第一拍就接管：INIT 只表示「还没跑起来」，不是一种运行策略。 */
    ensure_mode(fsm, (fsm->mode == FAN_MODE_INIT) ? FAN_MODE_AUTO : fsm->mode, now_ms);

    /* ---- 事件 ---- */
    if (event != NULL) {
        switch (event->type) {
        case FAN_EVENT_KEY_PRESS:
            if ((uint32_t)event->key < (uint32_t)FAN_KEY_COUNT) {
                fsm->key_pressed[event->key]    = true;
                fsm->key_press_tick[event->key] = now_ms;
                fsm->key_long_fired[event->key] = false;
            }
            break;

        case FAN_EVENT_KEY_RELEASE:
            if ((uint32_t)event->key < (uint32_t)FAN_KEY_COUNT) {
                /* 短按 = 松开时还没触发过长按 */
                if (fsm->key_pressed[event->key] && !fsm->key_long_fired[event->key]) {
                    handle_key_short(fsm, event->key);
                }
                fsm->key_pressed[event->key]    = false;
                fsm->key_long_fired[event->key] = false;
            }
            break;

        case FAN_EVENT_CMD:
            handle_command(fsm, event->cmd, event->value, now_ms, auto_duty);
            break;

        case FAN_EVENT_SENSOR_UPDATE:
        case FAN_EVENT_NONE:
        default:
            break;
        }
    }

    /* ---- 长按检查：每拍都做，不依赖本拍有没有事件 ---- */
    for (uint8_t k = 0U; k < (uint8_t)FAN_KEY_COUNT; k++) {
        if (fsm->key_pressed[k] && !fsm->key_long_fired[k]
            && ((uint32_t)(now_ms - fsm->key_press_tick[k]) >= fsm->cfg.long_press_ms)) {
            fsm->key_long_fired[k] = true;
            handle_key_long(fsm, (fan_key_t)k, now_ms, auto_duty);
        }
    }

    /* ---- 传感器降级与报警 ---- */
    update_degradation(fsm, temp_valid, tach_valid, now_ms);

    /* ---- 输出 ---- */
    uint8_t duty;

    switch (fsm->mode) {
    case FAN_MODE_MANUAL:
        duty = fsm->manual_duty;
        break;
    case FAN_MODE_AUTO:
    case FAN_MODE_DEGRADED:
        duty = auto_duty;
        break;
    case FAN_MODE_FAULT:
        duty = FAN_FSM_FULL_DUTY_PERCENT;
        break;
    case FAN_MODE_OFF:
    case FAN_MODE_INIT:
    default:
        duty = 0U;
        break;
    }

    const bool fault       = (fsm->mode == FAN_MODE_FAULT);
    const fan_alarm_t total = fsm->alarms | fsm->held_alarms;

    out->mode                = fsm->mode;
    out->duty_percent        = duty;
    out->alarm_lamp_on       = fault;
    out->buzzer_on           = fault && !fsm->muted;
    out->alarms              = total;
    out->new_alarms          = total & ~fsm->prev_total;
    out->temp_offset_milli_c = fsm->temp_offset_milli_c;
    out->manual_duty_percent = fsm->manual_duty;

    fsm->prev_total = total;

    return ERR_OK;
}

error_t fan_fsm_set_external_alarms(fan_fsm_t *fsm, fan_alarm_t alarms)
{
    if (fsm == NULL) {
        return ERR_INVALID_PARAM;
    }

    fsm->held_alarms = alarms;

    return ERR_OK;
}

fan_mode_t fan_fsm_get_mode(const fan_fsm_t *fsm)
{
    return (fsm == NULL) ? FAN_MODE_INIT : fsm->mode;
}
