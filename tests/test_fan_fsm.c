/**
 * @file test_fan_fsm.c
 * @brief module/fan_fsm 的宿主机单元测试。
 *
 * 状态机的价值在于「把事件序列想清楚」。板子上要复现「长按 800 ms 切模式」
 * 或「降级之后钥匙是否还被接受」，得真的按住按钮数秒；这里直接把时间轴摆出来，
 * 每个断言对应迁移表里的一格。
 */

#include "unity.h"

#include <stdbool.h>
#include <stdint.h>

#include "interface/error.h"
#include "module/common/control_types.h"
#include "module/fan_fsm/fan_fsm.h"

void setUp(void) {}
void tearDown(void) {}

/* ------------------------------------------------------------ 测试装置 */

static fan_fsm_config_t base_config(void)
{
    fan_fsm_config_t cfg = {
        .long_press_ms       = 800U,
        .min_dwell_ms        = 500U,
        .temp_step_milli_c   = 500,
        .temp_min_milli_c    = -10000,
        .temp_max_milli_c    = 10000,
        .manual_min_percent  = 20U,
    };

    return cfg;
}

/**
 * @brief base_config() 的「可取址」版本。
 *
 * C 里不能对函数返回值做强转再取址（转型结果不是左值），而多数用例只需一份
 * 默认配置，于是固化到一个文件级实例上。
 */
static fan_fsm_config_t g_default_config;

static fan_fsm_config_t *fresh_config(void)
{
    g_default_config = base_config();
    return &g_default_config;
}

static fan_event_t ev_key_press(fan_key_t key, uint32_t tick)
{
    fan_event_t e = {0};

    e.type    = FAN_EVENT_KEY_PRESS;
    e.key     = key;
    e.tick_ms = tick;

    return e;
}

static fan_event_t ev_key_release(fan_key_t key, uint32_t tick)
{
    fan_event_t e = {0};

    e.type    = FAN_EVENT_KEY_RELEASE;
    e.key     = key;
    e.tick_ms = tick;

    return e;
}

static fan_event_t ev_cmd(fan_cmd_t cmd, int32_t value, uint32_t tick)
{
    fan_event_t e = {0};

    e.type    = FAN_EVENT_CMD;
    e.cmd     = cmd;
    e.value   = value;
    e.tick_ms = tick;

    return e;
}

/** 推一拍（无事件）。 */
static void step(fan_fsm_t *fsm, uint32_t tick, uint8_t auto_duty,
                 bool temp_valid, bool tach_valid, fan_fsm_output_t *out)
{
    TEST_ASSERT_EQUAL_INT(ERR_OK,
                          fan_fsm_step(fsm, NULL, tick, auto_duty, temp_valid, tach_valid, out));
}

/** 推一拍（带事件）。 */
static void step_ev(fan_fsm_t *fsm, const fan_event_t *e, uint32_t tick, uint8_t auto_duty,
                    bool temp_valid, bool tach_valid, fan_fsm_output_t *out)
{
    TEST_ASSERT_EQUAL_INT(ERR_OK,
                          fan_fsm_step(fsm, e, tick, auto_duty, temp_valid, tach_valid, out));
}

/* ---------------------------------------------------------- 装载与校验 */

static void test_init_rejects_bad_configuration(void)
{
    fan_fsm_t        fsm = {0};
    fan_fsm_config_t cfg = base_config();

    TEST_ASSERT_EQUAL_INT(ERR_INVALID_PARAM, fan_fsm_init(NULL, &cfg));
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_PARAM, fan_fsm_init(&fsm, NULL));

    cfg = base_config();
    cfg.temp_min_milli_c = 1000;
    cfg.temp_max_milli_c = -1000;
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_PARAM, fan_fsm_init(&fsm, &cfg));

    cfg = base_config();
    cfg.manual_min_percent = 101U;
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_PARAM, fan_fsm_init(&fsm, &cfg));
}

static void test_step_before_init_is_not_initialized(void)
{
    fan_fsm_t        fsm = {0};
    fan_fsm_output_t out = {0};

    TEST_ASSERT_EQUAL_INT(ERR_NOT_INITIALIZED,
                          fan_fsm_step(&fsm, NULL, 0U, 0U, true, true, &out));
}

/** INIT 只是「还没跑起来」，第一拍就该接管成 AUTO。 */
static void test_first_step_takes_over_as_auto(void)
{
    fan_fsm_t        fsm = {0};
    fan_fsm_output_t out = {0};

    TEST_ASSERT_EQUAL_INT(ERR_OK, fan_fsm_init(&fsm, fresh_config()));

    step(&fsm, 100U, 55U, true, true, &out);

    TEST_ASSERT_EQUAL_INT(FAN_MODE_AUTO, out.mode);
    TEST_ASSERT_EQUAL_UINT8(55U, out.duty_percent);
    TEST_ASSERT_FALSE(out.alarm_lamp_on);
    TEST_ASSERT_FALSE(out.buzzer_on);
}

/* ---------------------------------------------------------- 短按调参 */

static void test_short_press_adjusts_target_temperature_in_auto(void)
{
    fan_fsm_t        fsm = {0};
    fan_fsm_output_t out = {0};
    fan_event_t      e;

    TEST_ASSERT_EQUAL_INT(ERR_OK, fan_fsm_init(&fsm, fresh_config()));
    step(&fsm, 100U, 40U, true, true, &out);

    e = ev_key_press(FAN_KEY_1, 200U);
    step_ev(&fsm, &e, 200U, 40U, true, true, &out);
    e = ev_key_release(FAN_KEY_1, 300U);
    step_ev(&fsm, &e, 300U, 40U, true, true, &out);
    TEST_ASSERT_EQUAL_INT32(500, out.temp_offset_milli_c);

    e = ev_key_press(FAN_KEY_2, 400U);
    step_ev(&fsm, &e, 400U, 40U, true, true, &out);
    e = ev_key_release(FAN_KEY_2, 450U);
    step_ev(&fsm, &e, 450U, 40U, true, true, &out);
    TEST_ASSERT_EQUAL_INT32(0, out.temp_offset_milli_c);
}

/** 温度偏移要被夹在配置区间内，不能靠连按把它推到荒谬的值。 */
static void test_temp_offset_is_clamped(void)
{
    fan_fsm_t        fsm = {0};
    fan_fsm_output_t out = {0};
    fan_event_t      e;

    TEST_ASSERT_EQUAL_INT(ERR_OK, fan_fsm_init(&fsm, fresh_config()));
    step(&fsm, 100U, 0U, true, true, &out);

    e = ev_cmd(FAN_CMD_TEMP_OFFSET_SET, 999999, 200U);
    step_ev(&fsm, &e, 200U, 0U, true, true, &out);
    TEST_ASSERT_EQUAL_INT32(10000, out.temp_offset_milli_c);

    e = ev_cmd(FAN_CMD_TEMP_OFFSET_SET, -999999, 300U);
    step_ev(&fsm, &e, 300U, 0U, true, true, &out);
    TEST_ASSERT_EQUAL_INT32(-10000, out.temp_offset_milli_c);
}

/* ---------------------------------------------------------- 长按切模式 */

static void test_long_press_cycles_mode(void)
{
    fan_fsm_t        fsm = {0};
    fan_fsm_output_t out = {0};
    fan_event_t      e;

    TEST_ASSERT_EQUAL_INT(ERR_OK, fan_fsm_init(&fsm, fresh_config()));
    step(&fsm, 100U, 40U, true, true, &out);
    TEST_ASSERT_EQUAL_INT(FAN_MODE_AUTO, out.mode);

    /* 按下 K1，在阈值之前不该有动作 */
    e = ev_key_press(FAN_KEY_1, 200U);
    step_ev(&fsm, &e, 200U, 40U, true, true, &out);
    step(&fsm, 700U, 40U, true, true, &out);          /* 200 + 500 < 800 */
    TEST_ASSERT_EQUAL_INT(FAN_MODE_AUTO, out.mode);

    /* 到达 800 ms → 切到手动档，且用当前自动值播种，免得一进手动就停 */
    step(&fsm, 1000U, 40U, true, true, &out);
    TEST_ASSERT_EQUAL_INT(FAN_MODE_MANUAL, out.mode);
    TEST_ASSERT_EQUAL_UINT8(40U, out.manual_duty_percent);
    TEST_ASSERT_EQUAL_UINT8(40U, out.duty_percent);

    /* 松开时不该再把这次长按当成短按处理 */
    e = ev_key_release(FAN_KEY_1, 1100U);
    step_ev(&fsm, &e, 1100U, 40U, true, true, &out);
    TEST_ASSERT_EQUAL_UINT8(40U, out.manual_duty_percent);   /* 没被 +10 */
}

/** 最小驻留：刚切完模式就再来一次用户操作，应被挡下，避免连按导致模式闪烁。 */
static void test_min_dwell_blocks_rapid_mode_switch(void)
{
    fan_fsm_t        fsm = {0};
    fan_fsm_output_t out = {0};
    fan_event_t      e;

    TEST_ASSERT_EQUAL_INT(ERR_OK, fan_fsm_init(&fsm, fresh_config()));
    step(&fsm, 100U, 0U, true, true, &out);

    /* 进手动档（t=1000），驻留计时从此刻起 */
    e = ev_key_press(FAN_KEY_1, 200U);
    step_ev(&fsm, &e, 200U, 0U, true, true, &out);
    step(&fsm, 1000U, 0U, true, true, &out);
    TEST_ASSERT_EQUAL_INT(FAN_MODE_MANUAL, out.mode);

    /* 立刻要求回 AUTO（距上次切换仅 50 ms）→ 应被拒绝 */
    e = ev_cmd(FAN_CMD_MODE_AUTO, 0, 1050U);
    step_ev(&fsm, &e, 1050U, 0U, true, true, &out);
    TEST_ASSERT_EQUAL_INT(FAN_MODE_MANUAL, out.mode);

    /* 过了驻留窗再要求 → 接受 */
    e = ev_cmd(FAN_CMD_MODE_AUTO, 0, 1600U);
    step_ev(&fsm, &e, 1600U, 0U, true, true, &out);
    TEST_ASSERT_EQUAL_INT(FAN_MODE_AUTO, out.mode);
}

/* -------------------------------------------------------------- 手动档 */

static void test_manual_duty_stepping_avoids_undefined_band(void)
{
    fan_fsm_t        fsm = {0};
    fan_fsm_output_t out = {0};
    fan_event_t      e;

    TEST_ASSERT_EQUAL_INT(ERR_OK, fan_fsm_init(&fsm, fresh_config()));
    step(&fsm, 100U, 50U, true, true, &out);

    /*
     * ⚠️ 上电接管（INIT→AUTO）也从那一刻开始计最小驻留，所以紧跟着的
     *    模式切换会被防抖挡下。这里刻意越过 500 ms 的驻留窗再切，
     *    否则测的就不是「切模式」而是「驻留防抖」了。
     */
    e = ev_cmd(FAN_CMD_MODE_MANUAL, 0, 700U);
    step_ev(&fsm, &e, 700U, 50U, true, true, &out);
    TEST_ASSERT_EQUAL_INT(FAN_MODE_MANUAL, out.mode);
    TEST_ASSERT_EQUAL_UINT8(50U, out.manual_duty_percent);   /* 播种自 auto_duty */

    /* 连按 K2 减档：50 → 40 → 30 → 20 → 0（不允许停在 10） */
    const uint8_t expected[] = { 40U, 30U, 20U, 0U };
    uint32_t      tick       = 800U;

    for (unsigned i = 0U; i < (sizeof(expected) / sizeof(expected[0])); i++) {
        e = ev_key_press(FAN_KEY_2, tick);
        step_ev(&fsm, &e, tick, 50U, true, true, &out);
        tick += 50U;
        e = ev_key_release(FAN_KEY_2, tick);
        step_ev(&fsm, &e, tick, 50U, true, true, &out);
        tick += 50U;

        TEST_ASSERT_EQUAL_UINT8(expected[i], out.manual_duty_percent);
    }

    /* 从 0 加档：0 → 20（跳过未定义的 10） */
    e = ev_key_press(FAN_KEY_1, tick);
    step_ev(&fsm, &e, tick, 50U, true, true, &out);
    tick += 50U;
    e = ev_key_release(FAN_KEY_1, tick);
    step_ev(&fsm, &e, tick, 50U, true, true, &out);

    TEST_ASSERT_EQUAL_UINT8(20U, out.manual_duty_percent);
}

static void test_manual_set_clamps_into_valid_positions(void)
{
    fan_fsm_t        fsm = {0};
    fan_fsm_output_t out = {0};
    fan_event_t      e;

    TEST_ASSERT_EQUAL_INT(ERR_OK, fan_fsm_init(&fsm, fresh_config()));
    step(&fsm, 100U, 0U, true, true, &out);

    /* 越过上电后 500 ms 的最小驻留窗，模式切换才会被接受 */
    e = ev_cmd(FAN_CMD_MODE_MANUAL, 0, 700U);
    step_ev(&fsm, &e, 700U, 0U, true, true, &out);
    TEST_ASSERT_EQUAL_INT(FAN_MODE_MANUAL, out.mode);

    /* 想要 8%：落在未定义区，应被抬到 20% */
    e = ev_cmd(FAN_CMD_MANUAL_SET, 8, 800U);
    step_ev(&fsm, &e, 800U, 0U, true, true, &out);
    TEST_ASSERT_EQUAL_UINT8(20U, out.manual_duty_percent);

    /* 想要 150%：夹到 100% */
    e = ev_cmd(FAN_CMD_MANUAL_SET, 150, 900U);
    step_ev(&fsm, &e, 900U, 0U, true, true, &out);
    TEST_ASSERT_EQUAL_UINT8(100U, out.manual_duty_percent);

    /* 想要 0%：允许，就是停 */
    e = ev_cmd(FAN_CMD_MANUAL_SET, 0, 1000U);
    step_ev(&fsm, &e, 1000U, 0U, true, true, &out);
    TEST_ASSERT_EQUAL_UINT8(0U, out.manual_duty_percent);
}

/* ------------------------------------------------------------ 降级路径 */

static void test_auto_degrades_on_single_sensor_failure(void)
{
    fan_fsm_t        fsm = {0};
    fan_fsm_output_t out = {0};

    TEST_ASSERT_EQUAL_INT(ERR_OK, fan_fsm_init(&fsm, fresh_config()));

    step(&fsm, 100U, 50U, true, true, &out);
    TEST_ASSERT_EQUAL_INT(FAN_MODE_AUTO, out.mode);

    /* 温度失效 → 降级，但仍按控制器给的安全值运行 */
    step(&fsm, 200U, 60U, false, true, &out);
    TEST_ASSERT_EQUAL_INT(FAN_MODE_DEGRADED, out.mode);
    TEST_ASSERT_EQUAL_UINT8(60U, out.duty_percent);
    TEST_ASSERT_TRUE((out.alarms & FAN_ALARM_TEMP_INVALID) != 0U);
    TEST_ASSERT_TRUE((out.new_alarms & FAN_ALARM_TEMP_INVALID) != 0U);
    TEST_ASSERT_FALSE(out.alarm_lamp_on);

    /* 恢复正常 → 回到 AUTO */
    step(&fsm, 300U, 50U, true, true, &out);
    TEST_ASSERT_EQUAL_INT(FAN_MODE_AUTO, out.mode);
    TEST_ASSERT_EQUAL_UINT32(0U, out.alarms & FAN_ALARM_TEMP_INVALID);
}

static void test_both_sensors_failing_enters_fault(void)
{
    fan_fsm_t        fsm = {0};
    fan_fsm_output_t out = {0};

    TEST_ASSERT_EQUAL_INT(ERR_OK, fan_fsm_init(&fsm, fresh_config()));

    step(&fsm, 100U, 50U, false, false, &out);

    TEST_ASSERT_EQUAL_INT(FAN_MODE_FAULT, out.mode);
    TEST_ASSERT_EQUAL_UINT8(100U, out.duty_percent);   /* 失效安全 = 满速 */
    TEST_ASSERT_TRUE(out.alarm_lamp_on);
    TEST_ASSERT_TRUE(out.buzzer_on);
}

/** FAULT 是粘性的：传感器恢复了也不自动退出，必须显式复位。 */
static void test_fault_is_sticky_until_reset(void)
{
    fan_fsm_t        fsm = {0};
    fan_fsm_output_t out = {0};
    fan_event_t      e;

    TEST_ASSERT_EQUAL_INT(ERR_OK, fan_fsm_init(&fsm, fresh_config()));
    step(&fsm, 100U, 50U, false, false, &out);
    TEST_ASSERT_EQUAL_INT(FAN_MODE_FAULT, out.mode);

    /* 传感器恢复 */
    step(&fsm, 200U, 50U, true, true, &out);
    TEST_ASSERT_EQUAL_INT(FAN_MODE_FAULT, out.mode);

    /* K1 长按在 FAULT 下必须被忽略 */
    e = ev_key_press(FAN_KEY_1, 300U);
    step_ev(&fsm, &e, 300U, 50U, true, true, &out);
    step(&fsm, 1200U, 50U, true, true, &out);
    TEST_ASSERT_EQUAL_INT(FAN_MODE_FAULT, out.mode);

    /* K2 长按 = 复位 */
    e = ev_key_press(FAN_KEY_2, 1300U);
    step_ev(&fsm, &e, 1300U, 50U, true, true, &out);
    step(&fsm, 2200U, 50U, true, true, &out);

    TEST_ASSERT_EQUAL_INT(FAN_MODE_AUTO, out.mode);
    TEST_ASSERT_FALSE(out.alarm_lamp_on);
    TEST_ASSERT_FALSE(out.buzzer_on);
}

/** 传感器没恢复时复位，应立刻又被打回 FAULT —— 复位不能凭空创造可用性。 */
static void test_reset_does_not_clear_still_broken_sensors(void)
{
    fan_fsm_t        fsm = {0};
    fan_fsm_output_t out = {0};
    fan_event_t      e;

    TEST_ASSERT_EQUAL_INT(ERR_OK, fan_fsm_init(&fsm, fresh_config()));
    step(&fsm, 100U, 50U, false, false, &out);

    e = ev_cmd(FAN_CMD_RESET, 0, 200U);
    step_ev(&fsm, &e, 200U, 50U, false, false, &out);

    TEST_ASSERT_EQUAL_INT(FAN_MODE_FAULT, out.mode);
}

/** 手动档下传感器失效只报警、不改策略——用户已经接管了输出。 */
static void test_manual_mode_ignores_sensor_failure(void)
{
    fan_fsm_t        fsm = {0};
    fan_fsm_output_t out = {0};
    fan_event_t      e;

    TEST_ASSERT_EQUAL_INT(ERR_OK, fan_fsm_init(&fsm, fresh_config()));
    step(&fsm, 100U, 40U, true, true, &out);

    /* 越过上电后 500 ms 的最小驻留窗 */
    e = ev_cmd(FAN_CMD_MODE_MANUAL, 0, 700U);
    step_ev(&fsm, &e, 700U, 40U, true, true, &out);
    TEST_ASSERT_EQUAL_INT(FAN_MODE_MANUAL, out.mode);

    step(&fsm, 800U, 40U, false, false, &out);
    TEST_ASSERT_EQUAL_INT(FAN_MODE_MANUAL, out.mode);
    TEST_ASSERT_EQUAL_UINT8(40U, out.duty_percent);
    TEST_ASSERT_TRUE((out.alarms & FAN_ALARM_TEMP_INVALID) != 0U);
}

/* ---------------------------------------------------------- OFF 档与灯 */

static void test_off_mode_stops_output_and_lamp(void)
{
    fan_fsm_t        fsm = {0};
    fan_fsm_output_t out = {0};
    fan_event_t      e;

    TEST_ASSERT_EQUAL_INT(ERR_OK, fan_fsm_init(&fsm, fresh_config()));
    step(&fsm, 100U, 40U, true, true, &out);

    /* 越过上电后 500 ms 的最小驻留窗 */
    e = ev_cmd(FAN_CMD_MODE_OFF, 0, 700U);
    step_ev(&fsm, &e, 700U, 40U, true, true, &out);

    TEST_ASSERT_EQUAL_INT(FAN_MODE_OFF, out.mode);
    TEST_ASSERT_EQUAL_UINT8(0U, out.duty_percent);
    TEST_ASSERT_FALSE(out.alarm_lamp_on);
}

/* -------------------------------------------------------------- 静音 */

static void test_buzzer_mute_suppresses_fault_buzzer(void)
{
    fan_fsm_t        fsm = {0};
    fan_fsm_output_t out = {0};
    fan_event_t      e;

    TEST_ASSERT_EQUAL_INT(ERR_OK, fan_fsm_init(&fsm, fresh_config()));
    step(&fsm, 100U, 40U, true, true, &out);

    /* K2 长按在非 FAULT 下切换静音 */
    e = ev_key_press(FAN_KEY_2, 200U);
    step_ev(&fsm, &e, 200U, 40U, true, true, &out);
    step(&fsm, 1100U, 40U, true, true, &out);

    /* 现在让传感器全挂 → FAULT，但蜂鸣器应为静音，报警灯仍亮 */
    step(&fsm, 1200U, 0U, false, false, &out);

    TEST_ASSERT_EQUAL_INT(FAN_MODE_FAULT, out.mode);
    TEST_ASSERT_TRUE(out.alarm_lamp_on);
    TEST_ASSERT_FALSE(out.buzzer_on);
}

/* ------------------------------------------------------ 外部报警透传 */

static void test_external_alarms_are_merged_into_output(void)
{
    fan_fsm_t        fsm = {0};
    fan_fsm_output_t out = {0};

    TEST_ASSERT_EQUAL_INT(ERR_OK, fan_fsm_init(&fsm, fresh_config()));
    TEST_ASSERT_EQUAL_INT(ERR_OK,
                          fan_fsm_set_external_alarms(&fsm,
                                                      FAN_ALARM_TEMP_UNCALIBRATED
                                                      | FAN_ALARM_OLED_ABSENT));

    step(&fsm, 100U, 40U, true, true, &out);

    TEST_ASSERT_TRUE((out.alarms & FAN_ALARM_TEMP_UNCALIBRATED) != 0U);
    TEST_ASSERT_TRUE((out.alarms & FAN_ALARM_OLED_ABSENT) != 0U);
    /* 外部报警不该改变模式：屏不在也得照常控温 */
    TEST_ASSERT_EQUAL_INT(FAN_MODE_AUTO, out.mode);
}

/* ---------------------------------------------------------------- main */

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_init_rejects_bad_configuration);
    RUN_TEST(test_step_before_init_is_not_initialized);
    RUN_TEST(test_first_step_takes_over_as_auto);

    RUN_TEST(test_short_press_adjusts_target_temperature_in_auto);
    RUN_TEST(test_temp_offset_is_clamped);

    RUN_TEST(test_long_press_cycles_mode);
    RUN_TEST(test_min_dwell_blocks_rapid_mode_switch);

    RUN_TEST(test_manual_duty_stepping_avoids_undefined_band);
    RUN_TEST(test_manual_set_clamps_into_valid_positions);

    RUN_TEST(test_auto_degrades_on_single_sensor_failure);
    RUN_TEST(test_both_sensors_failing_enters_fault);
    RUN_TEST(test_fault_is_sticky_until_reset);
    RUN_TEST(test_reset_does_not_clear_still_broken_sensors);
    RUN_TEST(test_manual_mode_ignores_sensor_failure);

    RUN_TEST(test_off_mode_stops_output_and_lamp);
    RUN_TEST(test_buzzer_mute_suppresses_fault_buzzer);
    RUN_TEST(test_external_alarms_are_merged_into_output);

    return UNITY_END();
}
