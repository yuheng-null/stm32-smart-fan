/**
 * @file test_ir_remote.c
 * @brief module/ir_remote 的宿主机单元测试。
 *
 * 红外解码不适合「上板试出来」：一次误码要在一整帧里定位到具体是哪一位，
 * 在板子上几乎做不到。这里改成**合成波形**——按 NEC 的标称时序生成一串
 * 「边沿 + 时长」，喂给解码器，逐条核结果。同一段波形既能测正常帧，
 * 也能故意把某一段拉偏、丢一个沿、改一位，看解码器是不是**按预期地**失败。
 *
 * 这是本工程里「能在 PC 上单测」这条分层规矩收益最大的一处：
 * 硬件只负责把边沿连同时长交上来，剩下的全在纯计算里。
 */

#include "unity.h"

#include <stdbool.h>
#include <stdint.h>

#include "module/ir_remote/ir_remote.h"

void setUp(void) {}
void tearDown(void) {}

/* ------------------------------------------------------------ 测试装置 */

/** NEC 标称时序（微秒）。 */
#define NEC_LEADER_MARK     (9000U)
#define NEC_LEADER_SPACE    (4500U)
#define NEC_REPEAT_SPACE    (2250U)
#define NEC_BIT_MARK        (562U)
#define NEC_BIT0_SPACE      (562U)
#define NEC_BIT1_SPACE      (1687U)
#define NEC_IDLE_US         (30000U)   /* 帧前的空闲；解码器应当忽略它的时长 */

/**
 * 一次边沿：level 是**边沿之后**的电平，us 是这个电平**之前**持续的微秒数。
 * 与 ir_remote_feed() 的参数一一对应，不在这里再做任何转换。
 */
typedef struct {
    bool     level;
    uint32_t us;
} edge_t;

/* 一帧数据 68 个沿（1 首沿 + 引导 2 + 32 位 ×2 + 收尾 1），留够放两帧。 */
#define WF_MAX_EDGES (192U)

typedef struct {
    edge_t   e[WF_MAX_EDGES];
    uint32_t n;
} waveform_t;

static void wf_reset(waveform_t *w)
{
    w->n = 0U;
}

static void wf_add(waveform_t *w, bool level_after, uint32_t us)
{
    TEST_ASSERT_TRUE_MESSAGE(w->n < WF_MAX_EDGES, "波形太长，加大 WF_MAX_EDGES");
    w->e[w->n].level = level_after;
    w->e[w->n].us    = us;
    w->n++;
}

/**
 * @brief 按 NEC 时序生成一帧数据。
 *
 * 信号从「空闲高」开始：第一个边沿把线拉低、开启引导 mark。之后每个边沿
 * 都带上刚过去的那一段时长，所以生成顺序与 ir_remote_feed 的调用顺序一致。
 *
 * @param addr       地址（extended 为假时只用低 8 位）。
 * @param cmd        命令字节。
 * @param extended   true 生成扩展 NEC（第 2 字节不是第 1 字节的反码）。
 * @param scale_pct  所有时长按此百分比缩放，用来测容差（100 = 标称）。
 * @param idles      在帧前插入的空闲时长。
 */
static void wf_build_frame(waveform_t *w, uint16_t addr, uint8_t cmd, bool extended,
                           uint32_t scale_pct, uint32_t idle_us)
{
    const uint32_t scale = scale_pct;

#define S(v) ((uint32_t)(((uint64_t)(v) * scale) / 100U))

    const uint8_t addr_lo = (uint8_t)(addr & 0xFFU);
    const uint8_t addr_hi = extended ? (uint8_t)((addr >> 8U) & 0xFFU)
                                     : (uint8_t)~addr_lo;
    const uint8_t cmd_inv = (uint8_t)~cmd;

    /* 32 位按 NEC 的发送顺序排：低位先发，所以第 n 位就是 word 的第 n 位。 */
    const uint32_t word = (uint32_t)addr_lo
                        | ((uint32_t)addr_hi << 8U)
                        | ((uint32_t)cmd << 16U)
                        | ((uint32_t)cmd_inv << 24U);

    wf_add(w, false, idle_us);              /* 高 → 低：引导 mark 开始 */
    wf_add(w, true, S(NEC_LEADER_MARK));    /* 低 → 高：引导 mark 结束 */
    wf_add(w, false, S(NEC_LEADER_SPACE));  /* 高 → 低：引导 space 结束 */

    for (uint32_t i = 0U; i < IR_NEC_BIT_COUNT; i++) {
        wf_add(w, true, S(NEC_BIT_MARK));   /* 低 → 高：这一位的 mark 结束 */
        const bool one = ((word >> i) & 1U) != 0U;
        wf_add(w, false, S(one ? NEC_BIT1_SPACE : NEC_BIT0_SPACE));
    }

    /* 收尾 mark：这一沿之后线上恢复空闲高 */
    wf_add(w, true, S(NEC_BIT_MARK));

#undef S
}

/** @brief 生成一帧重复码（按住不放时每 ~110 ms 一次）。 */
static void wf_build_repeat(waveform_t *w, uint32_t idle_us)
{
    wf_add(w, false, idle_us);
    wf_add(w, true, NEC_LEADER_MARK);
    wf_add(w, false, NEC_REPEAT_SPACE);
    wf_add(w, true, NEC_BIT_MARK);
}

/** @brief 把整段波形喂进去，返回解出的帧数。 */
static uint32_t wf_feed(ir_remote_t *ir, const waveform_t *w, ir_frame_t *last_out)
{
    uint32_t   count = 0U;
    ir_frame_t frame;

    for (uint32_t i = 0U; i < w->n; i++) {
        if (ir_remote_feed(ir, w->e[i].level, w->e[i].us, &frame)) {
            count++;
            if (last_out != NULL) {
                *last_out = frame;
            }
        }
    }

    return count;
}

/* ------------------------------------------------------------ 初始化 */

static void test_init_rejects_null(void)
{
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_PARAM, ir_remote_init(NULL));
}

static void test_stats_reject_null(void)
{
    ir_remote_t ir = {0};
    ir_stats_t  st;

    TEST_ASSERT_EQUAL_INT(ERR_OK, ir_remote_init(&ir));
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_PARAM, ir_remote_get_stats(NULL, &st));
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_PARAM, ir_remote_get_stats(&ir, NULL));
}

static void test_feed_null_is_harmless(void)
{
    TEST_ASSERT_FALSE(ir_remote_feed(NULL, true, 9000U, NULL));
}

/* ------------------------------------------------------------ 正常帧 */

static void test_decodes_standard_nec_frame(void)
{
    ir_remote_t  ir = {0};
    waveform_t   w;
    ir_frame_t   frame = {0};

    TEST_ASSERT_EQUAL_INT(ERR_OK, ir_remote_init(&ir));

    wf_reset(&w);
    wf_build_frame(&w, 0x00U, 0x45U, false, 100U, NEC_IDLE_US);

    TEST_ASSERT_EQUAL_UINT32(1U, wf_feed(&ir, &w, &frame));
    TEST_ASSERT_EQUAL_UINT16(0x00U, frame.address);
    TEST_ASSERT_EQUAL_UINT8(0x45U, frame.command);
    TEST_ASSERT_FALSE(frame.extended);
    TEST_ASSERT_FALSE(frame.is_repeat);
}

static void test_command_all_zeroes_and_all_ones(void)
{
    ir_remote_t ir = {0};
    waveform_t  w;
    ir_frame_t  frame = {0};

    TEST_ASSERT_EQUAL_INT(ERR_OK, ir_remote_init(&ir));

    /* 全 0：每一位都得判成 bit0，错一位就会被反码校验逮住 */
    wf_reset(&w);
    wf_build_frame(&w, 0x00U, 0x00U, false, 100U, NEC_IDLE_US);
    TEST_ASSERT_EQUAL_UINT32(1U, wf_feed(&ir, &w, &frame));
    TEST_ASSERT_EQUAL_UINT8(0x00U, frame.command);

    /* 全 1：同理，全部走 bit1 那条窗口 */
    wf_reset(&w);
    wf_build_frame(&w, 0x00U, 0xFFU, false, 100U, NEC_IDLE_US);
    TEST_ASSERT_EQUAL_UINT32(1U, wf_feed(&ir, &w, &frame));
    TEST_ASSERT_EQUAL_UINT8(0xFFU, frame.command);
}

static void test_bit_order_is_lsb_first(void)
{
    ir_remote_t ir = {0};
    waveform_t  w;
    ir_frame_t  frame = {0};

    TEST_ASSERT_EQUAL_INT(ERR_OK, ir_remote_init(&ir));

    /* 0xAA 与 0x55 是彼此的位反转：位序写反就会把两者弄混 */
    wf_reset(&w);
    wf_build_frame(&w, 0x12U, 0xAAU, false, 100U, NEC_IDLE_US);
    TEST_ASSERT_EQUAL_UINT32(1U, wf_feed(&ir, &w, &frame));
    TEST_ASSERT_EQUAL_UINT8(0xAAU, frame.command);
    TEST_ASSERT_EQUAL_UINT16(0x12U, frame.address);

    wf_reset(&w);
    wf_build_frame(&w, 0x12U, 0x55U, false, 100U, NEC_IDLE_US);
    TEST_ASSERT_EQUAL_UINT32(1U, wf_feed(&ir, &w, &frame));
    TEST_ASSERT_EQUAL_UINT8(0x55U, frame.command);
}

static void test_decodes_extended_address(void)
{
    ir_remote_t ir = {0};
    waveform_t  w;
    ir_frame_t  frame = {0};

    TEST_ASSERT_EQUAL_INT(ERR_OK, ir_remote_init(&ir));

    wf_reset(&w);
    wf_build_frame(&w, 0x34CDU, 0x0AU, true, 100U, NEC_IDLE_US);

    TEST_ASSERT_EQUAL_UINT32(1U, wf_feed(&ir, &w, &frame));
    TEST_ASSERT_TRUE(frame.extended);
    TEST_ASSERT_EQUAL_UINT16(0x34CDU, frame.address);
    TEST_ASSERT_EQUAL_UINT8(0x0AU, frame.command);
}

static void test_repeat_frame_is_recognized(void)
{
    ir_remote_t ir = {0};
    waveform_t  w;
    ir_frame_t  frame = {0};

    TEST_ASSERT_EQUAL_INT(ERR_OK, ir_remote_init(&ir));

    wf_reset(&w);
    wf_build_repeat(&w, NEC_IDLE_US);

    TEST_ASSERT_EQUAL_UINT32(1U, wf_feed(&ir, &w, &frame));
    TEST_ASSERT_TRUE(frame.is_repeat);
}

static void test_two_frames_back_to_back(void)
{
    ir_remote_t ir = {0};
    waveform_t  w;
    ir_frame_t  frame = {0};

    TEST_ASSERT_EQUAL_INT(ERR_OK, ir_remote_init(&ir));

    wf_reset(&w);
    wf_build_frame(&w, 0x00U, 0x45U, false, 100U, NEC_IDLE_US);
    wf_build_frame(&w, 0x00U, 0x46U, false, 100U, NEC_IDLE_US);

    TEST_ASSERT_EQUAL_UINT32(2U, wf_feed(&ir, &w, &frame));
    TEST_ASSERT_EQUAL_UINT8(0x46U, frame.command);
}

static void test_data_frame_then_repeat(void)
{
    ir_remote_t ir = {0};
    waveform_t  w;
    ir_frame_t  frame = {0};

    TEST_ASSERT_EQUAL_INT(ERR_OK, ir_remote_init(&ir));

    wf_reset(&w);
    wf_build_frame(&w, 0x00U, 0x45U, false, 100U, NEC_IDLE_US);
    wf_build_repeat(&w, 60000U);

    TEST_ASSERT_EQUAL_UINT32(2U, wf_feed(&ir, &w, &frame));
    TEST_ASSERT_TRUE(frame.is_repeat);
}

/* ------------------------------------------------------------ 容差 */

/**
 * 遥控器的晶振、发射芯片、接收头都有偏差，实际时长不会正好是标称值。
 * ±20% 以内必须照样解出来——窗口开得比这更宽，这里只是取一个下限。
 */
static void test_tolerates_timing_jitter(void)
{
    ir_remote_t ir = {0};
    waveform_t  w;
    ir_frame_t  frame = {0};

    TEST_ASSERT_EQUAL_INT(ERR_OK, ir_remote_init(&ir));

    const uint32_t scales[] = { 80U, 85U, 100U, 115U, 120U };

    for (uint32_t i = 0U; i < (sizeof(scales) / sizeof(scales[0])); i++) {
        wf_reset(&w);
        wf_build_frame(&w, 0x00U, 0x5AU, false, scales[i], NEC_IDLE_US);

        TEST_ASSERT_EQUAL_UINT32_MESSAGE(1U, wf_feed(&ir, &w, &frame),
                                         "标称值 ±20% 内的时长应当照常解出");
        TEST_ASSERT_EQUAL_UINT8(0x5AU, frame.command);
    }
}

/* ------------------------------------------------------------ 拒帧 */

static void test_rejects_implausible_leader_space(void)
{
    ir_remote_t ir = {0};
    waveform_t  w;
    ir_stats_t  st;

    TEST_ASSERT_EQUAL_INT(ERR_OK, ir_remote_init(&ir));

    wf_reset(&w);
    /* 引导 space 取 800 µs：既不像重复码的 2.25 ms，也不像引导的 4.5 ms */
    wf_add(&w, false, NEC_IDLE_US);
    wf_add(&w, true, NEC_LEADER_MARK);
    wf_add(&w, false, 800U);
    wf_add(&w, true, NEC_BIT_MARK);
    wf_add(&w, false, NEC_BIT0_SPACE);

    TEST_ASSERT_EQUAL_UINT32(0U, wf_feed(&ir, &w, NULL));

    TEST_ASSERT_EQUAL_INT(ERR_OK, ir_remote_get_stats(&ir, &st));
    TEST_ASSERT_EQUAL_UINT32(1U, st.rejects);
    TEST_ASSERT_EQUAL_INT(IR_REJECT_LEADER_SPACE, (int)st.last_reject);
    TEST_ASSERT_EQUAL_UINT32(0U, st.frames);
}

static void test_rejects_bit_mark_out_of_window(void)
{
    ir_remote_t ir = {0};
    waveform_t  w;
    ir_stats_t  st;

    TEST_ASSERT_EQUAL_INT(ERR_OK, ir_remote_init(&ir));

    wf_reset(&w);
    wf_add(&w, false, NEC_IDLE_US);
    wf_add(&w, true, NEC_LEADER_MARK);
    wf_add(&w, false, NEC_LEADER_SPACE);
    /* 第一位的 mark 拉成 3 ms：既不是 0.56 ms 的位标记，也不是 9 ms 的引导 */
    wf_add(&w, true, 3000U);

    TEST_ASSERT_EQUAL_UINT32(0U, wf_feed(&ir, &w, NULL));

    TEST_ASSERT_EQUAL_INT(ERR_OK, ir_remote_get_stats(&ir, &st));
    TEST_ASSERT_EQUAL_INT(IR_REJECT_BIT_MARK, (int)st.last_reject);
}

static void test_rejects_implausible_bit_space(void)
{
    ir_remote_t ir = {0};
    waveform_t  w;
    ir_stats_t  st;

    TEST_ASSERT_EQUAL_INT(ERR_OK, ir_remote_init(&ir));

    wf_reset(&w);
    wf_add(&w, false, NEC_IDLE_US);
    wf_add(&w, true, NEC_LEADER_MARK);
    wf_add(&w, false, NEC_LEADER_SPACE);
    wf_add(&w, true, NEC_BIT_MARK);
    wf_add(&w, false, 1090U);   /* 落在 bit0 上界 1050 与 bit1 下界 1150 之间 */

    TEST_ASSERT_EQUAL_UINT32(0U, wf_feed(&ir, &w, NULL));

    TEST_ASSERT_EQUAL_INT(ERR_OK, ir_remote_get_stats(&ir, &st));
    TEST_ASSERT_EQUAL_INT(IR_REJECT_BIT_SPACE, (int)st.last_reject);
}

/**
 * 反码校验是最后一道、也是最要紧的一道门：
 * 把命令字节里的一位改掉（但**不动**它的反码字节），整帧时长全部合规，
 * 只有校验能挡住它。
 */
static void test_checksum_catches_a_corrupted_command(void)
{
    ir_remote_t ir = {0};
    waveform_t  w;
    ir_stats_t  st;

    const uint8_t cmd = 0x45U;   /* 0x45 的反码是 0xBA */

    TEST_ASSERT_EQUAL_INT(ERR_OK, ir_remote_init(&ir));

    wf_reset(&w);

    /* 手工拼一帧：命令字节写 0x44（错了一位），反码字节仍按 0x45 写 */
    const uint32_t word = 0x00U
                        | ((uint32_t)(uint8_t)~0x00U << 8U)
                        | ((uint32_t)0x44U << 16U)
                        | ((uint32_t)(uint8_t)~cmd << 24U);

    wf_add(&w, false, NEC_IDLE_US);
    wf_add(&w, true, NEC_LEADER_MARK);
    wf_add(&w, false, NEC_LEADER_SPACE);

    for (uint32_t i = 0U; i < IR_NEC_BIT_COUNT; i++) {
        wf_add(&w, true, NEC_BIT_MARK);
        const bool one = ((word >> i) & 1U) != 0U;
        wf_add(&w, false, one ? NEC_BIT1_SPACE : NEC_BIT0_SPACE);
    }
    wf_add(&w, true, NEC_BIT_MARK);

    TEST_ASSERT_EQUAL_UINT32(0U, wf_feed(&ir, &w, NULL));

    TEST_ASSERT_EQUAL_INT(ERR_OK, ir_remote_get_stats(&ir, &st));
    TEST_ASSERT_EQUAL_INT(IR_REJECT_CHECKSUM, (int)st.last_reject);
    TEST_ASSERT_EQUAL_UINT32(1U, st.rejects);
}

/* ------------------------------------------------------------ 重同步 */

/**
 * 按键中途松开、或线上掉一个沿时，状态机会停在半路。此时**下一帧必须还能
 * 解出来**——这是解码器最容易写丢的一条，也是「按住不放」体验好坏的关键。
 */
static void test_next_frame_decodes_after_a_truncated_one(void)
{
    ir_remote_t ir = {0};
    waveform_t  w;
    ir_frame_t  frame = {0};

    TEST_ASSERT_EQUAL_INT(ERR_OK, ir_remote_init(&ir));

    wf_reset(&w);

    /* 半截帧：走完引导码与头 8 位就断掉（模拟中途松手） */
    wf_add(&w, false, NEC_IDLE_US);
    wf_add(&w, true, NEC_LEADER_MARK);
    wf_add(&w, false, NEC_LEADER_SPACE);
    for (uint32_t i = 0U; i < 8U; i++) {
        wf_add(&w, true, NEC_BIT_MARK);
        wf_add(&w, false, NEC_BIT0_SPACE);
    }

    /* 紧接着一帧完整的 */
    wf_build_frame(&w, 0x00U, 0x45U, false, 100U, NEC_IDLE_US);

    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1U, wf_feed(&ir, &w, &frame),
                                     "半截帧之后的完整帧必须能解出来");
    TEST_ASSERT_EQUAL_UINT8(0x45U, frame.command);
}

static void test_garbage_burst_then_valid_frame(void)
{
    ir_remote_t ir = {0};
    waveform_t  w;
    ir_frame_t  frame = {0};

    TEST_ASSERT_EQUAL_INT(ERR_OK, ir_remote_init(&ir));

    wf_reset(&w);

    /* 一串乱七八糟的短脉冲：像是日光灯/其它遥控器的干扰 */
    for (uint32_t i = 0U; i < 12U; i++) {
        wf_add(&w, (i % 2U) == 0U, 137U + (i * 41U));
    }

    wf_build_frame(&w, 0x00U, 0x07U, false, 100U, NEC_IDLE_US);

    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1U, wf_feed(&ir, &w, &frame),
                                     "干扰之后必须能重新锁上");
    TEST_ASSERT_EQUAL_UINT8(0x07U, frame.command);
}

/**
 * 上电后的第一个边沿带着「没有意义的时长」进来（谁也不知道线上高了多久）。
 * 它必须被无声地丢掉，而不是被当成帧的一部分。
 */
static void test_first_edge_with_meaningless_interval_is_ignored(void)
{
    ir_remote_t ir = {0};
    waveform_t  w;
    ir_frame_t  frame = {0};

    TEST_ASSERT_EQUAL_INT(ERR_OK, ir_remote_init(&ir));

    wf_reset(&w);
    /* 空闲先给一个很长的值；再给一个恰好落在引导 mark 窗口里的「假」首沿，
       它后面的形态对不上，于是被拒——不能因此吐出一帧。 */
    wf_add(&w, false, 0xFFFF0000UL);
    wf_add(&w, true, NEC_LEADER_MARK);
    wf_add(&w, false, 700U);            /* 假引导之后的 space 不合规 */

    TEST_ASSERT_EQUAL_UINT32(0U, wf_feed(&ir, &w, NULL));

    wf_reset(&w);
    wf_build_frame(&w, 0x00U, 0x45U, false, 100U, 0xFFFF0000UL);
    TEST_ASSERT_EQUAL_UINT32(1U, wf_feed(&ir, &w, &frame));
    TEST_ASSERT_EQUAL_UINT8(0x45U, frame.command);
}

/* ------------------------------------------------------------ 统计 */

static void test_stats_count_edges_frames_and_repeats(void)
{
    ir_remote_t ir = {0};
    waveform_t  w;
    ir_stats_t  st;

    TEST_ASSERT_EQUAL_INT(ERR_OK, ir_remote_init(&ir));

    wf_reset(&w);
    wf_build_frame(&w, 0x00U, 0x45U, false, 100U, NEC_IDLE_US);
    const uint32_t edges_in_frame = w.n;

    TEST_ASSERT_EQUAL_UINT32(1U, wf_feed(&ir, &w, NULL));

    wf_reset(&w);
    wf_build_frame(&w, 0x00U, 0x46U, false, 100U, NEC_IDLE_US);
    wf_build_repeat(&w, 60000U);

    TEST_ASSERT_EQUAL_UINT32(2U, wf_feed(&ir, &w, NULL));

    TEST_ASSERT_EQUAL_INT(ERR_OK, ir_remote_get_stats(&ir, &st));
    TEST_ASSERT_EQUAL_UINT32(2U, st.frames);
    TEST_ASSERT_EQUAL_UINT32(1U, st.repeats);
    TEST_ASSERT_EQUAL_UINT32(0U, st.rejects);
    TEST_ASSERT_EQUAL_UINT32((2U * edges_in_frame) + 4U, st.edges);
}

static void test_reset_clears_mid_frame_state(void)
{
    ir_remote_t ir = {0};
    waveform_t  w;
    ir_frame_t  frame = {0};

    TEST_ASSERT_EQUAL_INT(ERR_OK, ir_remote_init(&ir));

    /* 先收半截 */
    wf_reset(&w);
    wf_add(&w, false, NEC_IDLE_US);
    wf_add(&w, true, NEC_LEADER_MARK);
    wf_add(&w, false, NEC_LEADER_SPACE);
    TEST_ASSERT_EQUAL_UINT32(0U, wf_feed(&ir, &w, NULL));

    ir_remote_reset(&ir);

    /* 复位之后完整帧照收 */
    wf_reset(&w);
    wf_build_frame(&w, 0x00U, 0x45U, false, 100U, NEC_IDLE_US);
    TEST_ASSERT_EQUAL_UINT32(1U, wf_feed(&ir, &w, &frame));
    TEST_ASSERT_EQUAL_UINT8(0x45U, frame.command);
}

/* ---------------------------------------------------------------- main */

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_init_rejects_null);
    RUN_TEST(test_stats_reject_null);
    RUN_TEST(test_feed_null_is_harmless);

    RUN_TEST(test_decodes_standard_nec_frame);
    RUN_TEST(test_command_all_zeroes_and_all_ones);
    RUN_TEST(test_bit_order_is_lsb_first);
    RUN_TEST(test_decodes_extended_address);
    RUN_TEST(test_repeat_frame_is_recognized);
    RUN_TEST(test_two_frames_back_to_back);
    RUN_TEST(test_data_frame_then_repeat);

    RUN_TEST(test_tolerates_timing_jitter);

    RUN_TEST(test_rejects_implausible_leader_space);
    RUN_TEST(test_rejects_bit_mark_out_of_window);
    RUN_TEST(test_rejects_implausible_bit_space);
    RUN_TEST(test_checksum_catches_a_corrupted_command);

    RUN_TEST(test_next_frame_decodes_after_a_truncated_one);
    RUN_TEST(test_garbage_burst_then_valid_frame);
    RUN_TEST(test_first_edge_with_meaningless_interval_is_ignored);

    RUN_TEST(test_stats_count_edges_frames_and_repeats);
    RUN_TEST(test_reset_clears_mid_frame_state);

    return UNITY_END();
}
