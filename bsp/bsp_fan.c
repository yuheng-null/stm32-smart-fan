/**
 * @file bsp_fan.c
 * @brief 板级风扇实现：TIM1_CH1 出 25 kHz PWM，TIM2_CH2 捕获 FG 沿。
 */

#include "bsp/bsp_fan.h"

#include <stdbool.h>
#include <stddef.h>

#include "main.h"                   /* 借它引入 HAL 头与 htim1 / htim2 / 引脚宏 */

#include "bsp/bsp_time.h"           /* TIM2 的计数频率（它同时是全系统的 µs 时基） */

/*
 * ── 引脚与外设参数都在 CubeMX 里，不在本文件 ─────────────────────────
 *
 * .ioc 里已声明：PA8 = TIM1_CH1（AF 开漏，PSC=0 / ARR=6719 → 25 kHz），
 *                PA1 = TIM2_CH2（AF 上拉，PSC=83 → 1 MHz 计数）。
 * 本层只负责「起停」与「运行期改占空比」，不重新配置外设。
 * 理由同 bsp_key.c：这些是**本板的接线选择**，交给 CubeMX 统一管理，
 * 免得以后加外设时和它抢引脚（那是静默冲突）。
 * ────────────────────────────────────────────────────────────────────
 */

/**
 * 多久没有新的 FG 沿就认为风扇停了（毫秒）。
 *
 * 取 1000 ms 的依据：本层按「相邻两沿间隔」测速，间隔本身最长就是一个
 * 整的半转周期。1 s 的间隔对应 30 RPM（= 30e6 / 1e6），风扇不可能稳定转在
 * 这个转速还转得动；更慢的只可能是停转或 FG 线没信号。
 * 代价是风扇真停下后约 1 s 才报 0，可以接受。
 */
#define BSP_FAN_TACH_TIMEOUT_MS     (1000U)

/** 已初始化标志。 */
static bool s_ready = false;

/** 定时器计数频率（Hz）。init 时算一次，用于把计数换算成微秒。 */
static uint32_t s_tick_hz = 0U;

/* ---- 以下四个由 TIM2 中断写、由主上下文读，必须 volatile ---- */

/** 最近一次测得的「相邻两沿间隔」，单位是**定时器计数**（不是微秒）。 */
static volatile uint32_t s_period_ticks = 0U;

/** 是否已经凑够两个沿、s_period_ticks 有效。 */
static volatile bool s_have_period = false;

/** 上一次捕获的时刻（ms），用于判断信号是否已经断掉。 */
static volatile uint32_t s_last_capture_ms = 0U;

/** 累计捕获到的沿数（调试用）。 */
static volatile uint32_t s_capture_count = 0U;

/** 上一次捕获到的计数值。仅中断上下文访问，不必 volatile。 */
static uint32_t s_last_capture = 0U;

/** 是否已经捕获过至少一个沿。 */
static bool s_have_capture = false;

/** 当前占空比（百分比）。 */
static uint8_t s_duty_percent = 0U;

/* ----------------------------------------------------------------- 内部 */

/*
 * 占空比的合法区间（%）。
 *
 * 这里自己定义一份、而不是去引 module/fan_control 的同名宏：架构规定
 * `BSP → MODULE` 是**反向依赖**，两边互不认识。跨层共享一个数值不值得
 * 为此破例——真要统一，应当把它下沉到 interface/。
 */
#define BSP_FAN_DUTY_MIN_PERCENT    (0U)
#define BSP_FAN_DUTY_MAX_PERCENT    (100U)

/**
 * @brief 把占空比百分比折算成定时器比较值（CCR）。
 *
 * 这是**硬件映射**，不是业务逻辑，所以留在 BSP：
 * 百分比是给人看的，CCR 是本颗定时器的计数，两者的取整关系只在本层才有意义。
 *
 * 约定 100% 返回 `period_counts`（= ARR+1）。PWM 模式下 CCR 大于 ARR 时
 * 比较恒成立、输出整周期有效，这才是真正的 100%；若按比例算成
 * period_counts 也一样，但显式写出来能挡住越界取值带来的意外。
 *
 * @param percent       占空比百分比，调用前须已在 [0,100] 内。
 * @param period_counts 一个完整周期的计数个数（ARR + 1）。
 */
static uint32_t fan_duty_to_compare(uint8_t percent, uint32_t period_counts)
{
    if (period_counts == 0U) {
        return 0U;
    }

    if (percent >= BSP_FAN_DUTY_MAX_PERCENT) {
        return period_counts;
    }

    /* 先乘后除：反过来每次都会丢掉不足 1 个计数的余数，占空比系统性偏小。 */
    return ((uint32_t)percent * period_counts) / (uint32_t)BSP_FAN_DUTY_MAX_PERCENT;
}

/* ----------------------------------------------------------------- 对外 */

error_t bsp_fan_init(void)
{
    s_ready          = false;
    s_have_period    = false;
    s_have_capture   = false;
    s_period_ticks   = 0U;
    s_capture_count  = 0U;
    s_duty_percent   = 0U;

    /* ---- PWM 侧 ---- */
    /* 先把占空比压到 0 再启动，避免启动瞬间甩出去一个非零占空比 */
    (void)__HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, 0U);

    /*
     * HAL_TIM_PWM_Start() 对 TIM1 这种**高级定时器**还有一件必须做的事：
     * 置位 BDTR 的 MOE（主输出使能）。少了它，CCER 配得再对，引脚也不出波形
     * ——而且不报任何错。HAL 在 PWM_Start 里替我们做了，所以不要绕开它直接
     * 写 CCR。
     */
    if (HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_1) != HAL_OK) {
        return ERR_BUSY;
    }

    /* ---- 测速侧 ---- */
    /*
     * TIM2 的计数频率由 bsp_time 算：那个模块把同一个计数器当作全系统的
     * 微秒时基，两边共用一处换算，改 .ioc 的 PSC 时不会只有一边跟着变。
     * 它读 PSC 读的是 htim2.Init.Prescaler（HAL 在 HAL_TIM_IC_Init() 里
     * 把它写进 TIM2->PSC，两者恒等），所以 .ioc 改了也自洽。
     */
    s_tick_hz = bsp_time_counter_hz();
    if (s_tick_hz == 0U) {
        (void)HAL_TIM_PWM_Stop(&htim1, TIM_CHANNEL_1);
        return ERR_NOT_READY;
    }

    /*
     * 打开**捕获中断**而不是轮询：主循环每 200 ms 才走一圈，而 1000 RPM 下
     * 沿间隔是 30 ms —— 轮询会漏掉中间的沿，读到的间隔就变成「主循环周期」
     * 而不是「半转周期」，算出来的转速会小几倍，且看不出哪里错了。
     */
    if (HAL_TIM_IC_Start_IT(&htim2, TIM_CHANNEL_2) != HAL_OK) {
        (void)HAL_TIM_PWM_Stop(&htim1, TIM_CHANNEL_1);
        return ERR_BUSY;
    }

    s_ready = true;

    return ERR_OK;
}

error_t bsp_fan_set_duty_percent(uint8_t percent)
{
    uint32_t period_counts;
    uint32_t compare;

    if (!s_ready) {
        return ERR_NOT_INITIALIZED;
    }

    if (percent > BSP_FAN_DUTY_MAX_PERCENT) {
        return ERR_INVALID_PARAM;
    }

    /*
     * 一个完整周期的计数个数 = ARR + 1（计数器从 0 数到 ARR）。
     * 从寄存器读而不是写死 6720：.ioc 里改了 Period，这里自动跟上。
     */
    period_counts = (uint32_t)__HAL_TIM_GET_AUTORELOAD(&htim1) + 1U;

    compare = fan_duty_to_compare(percent, period_counts);

    (void)__HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, compare);

    s_duty_percent = percent;

    return ERR_OK;
}

uint8_t bsp_fan_get_duty_percent(void)
{
    return s_duty_percent;
}

error_t bsp_fan_get_period_us(uint32_t *period_us)
{
    uint32_t ticks;
    bool     valid;
    uint32_t primask;

    if (period_us == NULL) {
        return ERR_INVALID_PARAM;
    }
    if (!s_ready) {
        return ERR_NOT_INITIALIZED;
    }

    /*
     * s_have_period 与 s_period_ticks 是一对，若在中断恰好更新到一半时读取，
     * 可能拿到「新的有效标志 + 旧的计数值」。关几条指令的中断把它整体读完；
     * 中断里只是写两个变量，晚几十纳秒没有任何影响。
     */
    primask = __get_PRIMASK();
    __disable_irq();
    valid = s_have_period;
    ticks = s_period_ticks;
    __set_PRIMASK(primask);

    if (!valid) {
        /* 一个沿都还没凑够两条 —— 不是错误，只是还没数据 */
        return ERR_NOT_READY;
    }

    /* 信号断掉了吗？断了就当停转，不要拿几秒前的旧周期冒充当前转速。 */
    if ((uint32_t)(HAL_GetTick() - s_last_capture_ms) > BSP_FAN_TACH_TIMEOUT_MS) {
        return ERR_TIMEOUT;
    }

    /* 计数 → 微秒。先乘后除并把中间量放大到 64 位，避免 (ticks × 1e6) 溢出 32 位。 */
    *period_us = (uint32_t)(((uint64_t)ticks * 1000000ULL) / (uint64_t)s_tick_hz);

    return ERR_OK;
}

uint32_t bsp_fan_get_capture_count(void)
{
    return s_capture_count;
}

/**
 * @brief 把一根引脚从 AF 改成普通推挽输出并拉低。
 *
 * 顺序很重要：**先写 ODR，再切模式**。切到输出模式的那一刻 ODR 的值立刻
 * 生效，若反过来，这根线会先被甩出一个短暂的高电平——对本诊断来说正好
 * 是最坏的情况：风扇可能因此被"确认"成全速，把结论带偏。
 */
static void fan_force_one_low(GPIO_TypeDef *port, uint16_t pin)
{
    GPIO_InitTypeDef gpio = {0};

    HAL_GPIO_WritePin(port, pin, GPIO_PIN_RESET);

    gpio.Pin   = pin;
    gpio.Mode  = GPIO_MODE_OUTPUT_PP;
    gpio.Pull  = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;

    HAL_GPIO_Init(port, &gpio);
}

void bsp_fan_force_pin_low(bsp_fan_force_t which)
{
    if ((which == BSP_FAN_FORCE_PWM) || (which == BSP_FAN_FORCE_BOTH)) {
        (void)HAL_TIM_PWM_Stop(&htim1, TIM_CHANNEL_1);
        fan_force_one_low(FAN_PWM_GPIO_Port, FAN_PWM_Pin);
    }

    if ((which == BSP_FAN_FORCE_TACH) || (which == BSP_FAN_FORCE_BOTH)) {
        (void)HAL_TIM_IC_Stop_IT(&htim2, TIM_CHANNEL_2);
        fan_force_one_low(FAN_TACH_GPIO_Port, FAN_TACH_Pin);
    }

    s_ready = false;
}

/* ------------------------------------------------------- 中断回调（HAL） */

/**
 * @brief TIM2 捕获中断回调。
 *
 * 由 HAL_TIM_IRQHandler() 在 CC2IF 置位时调用（见 Core/Src/stm32f4xx_it.c
 * 的 TIM2_IRQHandler），运行在**中断上下文**。
 *
 * 「Fast In, Fast Out」：这里只做一次读寄存器 + 一次减法，
 * 换算成微秒、算转速都留给主上下文。ISR 里做乘除会拉长中断、
 * 影响其他中断的响应。
 */
void HAL_TIM_IC_CaptureCallback(TIM_HandleTypeDef *htim)
{
    uint32_t now;

    /* 回调是全局的，只认自己那条通道，其余一律不管 */
    if ((htim->Instance != TIM2) || (htim->Channel != HAL_TIM_ACTIVE_CHANNEL_2)) {
        return;
    }

    now = HAL_TIM_ReadCapturedValue(&htim2, TIM_CHANNEL_2);

    if (s_have_capture) {
        /*
         * 32 位无符号减法：定时器回绕（约 4295 秒一次）时结果依然正确，
         * 直接比较大小反而会在回绕点算出个巨大的间隔。
         */
        const uint32_t delta = now - s_last_capture;

        if (delta > 0U) {
            s_period_ticks = delta;
            s_have_period  = true;
        }
    }

    s_last_capture    = now;
    s_have_capture    = true;
    s_last_capture_ms = HAL_GetTick();
    s_capture_count++;
}
