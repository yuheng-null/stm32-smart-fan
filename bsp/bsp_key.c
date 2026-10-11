/**
 * @file bsp_key.c
 * @brief 板载 2 个按键的实现（1 ms 节拍采样 + 计数式消抖）。
 */

#include "bsp/bsp_key.h"

#include "main.h"                   /* 借它引入 stm32f4xx_hal.h 与引脚宏 */

/* ----------------------------------------------------------------- 配置 */

/*
 * ── 引脚的配置在 CubeMX 里，不在本文件 ──────────────────────────────
 *
 * .ioc 里 PC0 / PC1 已声明为「上拉输入」并打了 KEY1 / KEY2 标签，
 * 宏 KEY1_Pin / KEY1_GPIO_Port 由 CubeMX 生成（见 main.h），
 * 引脚模式由生成的 MX_GPIO_Init() 完成。
 *
 * 为什么不在这里自己配引脚：按键的「输入 + 上拉」只是**本板的接线选择**，
 * 没有任何协议含义，交给 CubeMX 统一管理更清楚——也避免以后在 CubeMX 里
 * 加外设时不小心把这两个脚分配出去，和按键抢引脚（那是静默冲突）。
 * 对比：软件 I2C 的「开漏 + 上拉」是协议的一部分，所以由总线驱动自己保证。
 * ────────────────────────────────────────────────────────────────────
 */

/**
 * 引脚被按下时读到的电平。
 *
 * 按键另一端接 GND + 引脚内部上拉 → 空闲为高、按下为低 → GPIO_PIN_RESET。
 * ⚠️ 若实际接线相反（按下接高电平），这里改成 GPIO_PIN_SET，
 *    并同步把 .ioc 里这两个引脚改成下拉。
 */
#define BSP_KEY_ACTIVE_LEVEL    (GPIO_PIN_RESET)

/*
 * 消抖时间（毫秒）。机械按键的弹跳一般持续 1~5 ms，取 20 ms 有充分余量，
 * 又不会让人觉得「按下去没反应」（人对 20 ms 无感，对 100 ms 就有感了）。
 *
 * 节拍就是 SysTick 的 1 ms，故毫秒数即采样次数。
 */
#define BSP_KEY_DEBOUNCE_MS     (20U)

/* ----------------------------------------------------------------- 类型 */

/** 引脚的物理位置，与 key_id_t 一一对应。 */
typedef struct {
    GPIO_TypeDef *port;
    uint16_t      pin;
} key_hw_t;

/**
 * 单个按键的消抖状态。
 *
 * **跨上下文访问的三个字段都必须 volatile**：stable 由 SysTick 中断写、
 * 主循环读，两个 pending 标志由中断写、由 bsp_key_take_event() 读并清除。
 * 常有人以「bool 是单字节、Cortex-M4 上单字节读写原子」为由省掉 volatile——
 * 原子性挡的是**撕裂**，挡不住**编译器把读取提到循环外缓存**。一旦将来开了
 * LTO 而读取被内联进循环，就可能一直看到陈旧电平。所以这里一个都没省。
 */
typedef struct {
    volatile bool stable;            /**< 消抖后的稳定状态（true = 按下） */
    bool          last_raw;          /**< 上一次原始采样（true = 按下）；仅 ISR 内访问 */
    uint16_t      same_count;        /**< 连续相同采样的次数；仅 ISR 内访问 */
    volatile bool press_pending;     /**< 有未取走的「按下」事件 */
    volatile bool release_pending;   /**< 有未取走的「松开」事件 */
} key_state_t;

/* ----------------------------------------------------------------- 状态 */

/* 引脚宏来自 CubeMX（main.h），顺序必须与 key_id_t 一一对应。 */
static const key_hw_t s_key_hw[KEY_ID_COUNT] = {
    { KEY1_GPIO_Port, KEY1_Pin },
    { KEY2_GPIO_Port, KEY2_Pin },
};

static key_state_t s_key_state[KEY_ID_COUNT];

/* ----------------------------------------------------------------- 内部 */

/** 读取引脚，返回「是否处于按下状态」。 */
static bool key_read_pressed(const key_hw_t *hw)
{
    return HAL_GPIO_ReadPin(hw->port, hw->pin) == BSP_KEY_ACTIVE_LEVEL;
}

/**
 * @brief 单个按键的消抖状态机，每 1 ms 推进一次。
 *
 * 思路：连续采集到 N 次相同的电平才承认它「稳定地变了」。
 * 中途只要电平又跳了，计数就清零重新来——毛刺因此永远攒不够 N 次。
 *
 * @note 在中断上下文执行，只做几个比较和自增，无阻塞、无函数调用开销。
 */
static void key_scan_one(const key_hw_t *hw, key_state_t *st)
{
    const bool raw = key_read_pressed(hw);

    if (raw != st->last_raw) {
        /* 电平刚变化，很可能是弹跳，重新计时 */
        st->last_raw   = raw;
        st->same_count = 0U;
        return;
    }

    if (st->same_count < BSP_KEY_DEBOUNCE_MS) {
        st->same_count++;

        /* 连续 N 次一致，且与已确认的状态不同 → 电平真的翻转了 */
        if ((st->same_count >= BSP_KEY_DEBOUNCE_MS) && (st->stable != raw)) {
            st->stable = raw;

            if (raw) {
                st->press_pending = true;
            } else {
                st->release_pending = true;
            }
        }
    }
}

/* ----------------------------------------------------------------- 对外 */

error_t bsp_key_init(void)
{
    /*
     * 本函数不配置引脚——那由 CubeMX 生成的 MX_GPIO_Init() 做
     * （输入 + 上拉，GPIOC 的时钟也在那里使能）。
     *
     * ⚠️ 因此**必须在 MX_GPIO_Init() 之后调用**。
     */
    for (uint8_t i = 0U; i < (uint8_t)KEY_ID_COUNT; i++) {
        /*
         * 用上电时的真实电平做初始状态。
         * 如果一律初始化成「松开」而实际是「按着」，第一次采样会被当成一次
         * 电平变化，20 ms 后凭空产生一个按下事件——开机就多计一次数。
         */
        const bool raw = key_read_pressed(&s_key_hw[i]);

        s_key_state[i].stable          = raw;
        s_key_state[i].last_raw        = raw;
        s_key_state[i].same_count      = BSP_KEY_DEBOUNCE_MS;  /* 已视为稳定 */
        s_key_state[i].press_pending   = false;
        s_key_state[i].release_pending = false;
    }

    return ERR_OK;
}

void bsp_key_scan_isr(void)
{
    for (uint8_t i = 0U; i < (uint8_t)KEY_ID_COUNT; i++) {
        key_scan_one(&s_key_hw[i], &s_key_state[i]);
    }
}

key_event_t bsp_key_take_event(key_id_t id)
{
    if ((uint32_t)id >= (uint32_t)KEY_ID_COUNT) {
        return KEY_EVENT_NONE;
    }

    key_state_t *st = &s_key_state[id];
    key_event_t  ev = KEY_EVENT_NONE;

    /*
     * 这两个标志由 SysTick 中断写、由本函数读写，属于跨上下文的共享数据。
     * 「判断 + 清除」若不加保护，恰好在两条语句之间发生的中断所置的标志
     * 会被下面那句清除掉，事件就丢了。这种 bug 一天可能只出现一次，
     * 极难复现，所以这里必须关中断。
     *
     * 只关几条指令的时间（几十纳秒），对 1 ms 节拍毫无影响。
     */
    const uint32_t primask = __get_PRIMASK();
    __disable_irq();

    if (st->press_pending) {
        st->press_pending = false;
        ev = KEY_EVENT_PRESS;
    } else if (st->release_pending) {
        st->release_pending = false;
        ev = KEY_EVENT_RELEASE;
    }

    /* 恢复关中断之前的状态，而不是无脑 __enable_irq()：
       这样即使本函数被从「已经关中断」的上下文里调用，也不会意外开中断。 */
    __set_PRIMASK(primask);

    return ev;
}

bool bsp_key_is_pressed(key_id_t id)
{
    if ((uint32_t)id >= (uint32_t)KEY_ID_COUNT) {
        return false;
    }

    /* stable 是单字节且已声明 volatile：读到的要么是旧值要么是新值，
       不会撕裂，也不会被编译器缓存——不必关中断。 */
    return s_key_state[id].stable;
}
