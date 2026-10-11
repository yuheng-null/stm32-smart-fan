/**
 * @file bsp_key.h
 * @brief 板载 2 个独立按键（K1/K2），带消抖。
 *
 * ── 接线 ────────────────────────────────────────────────────────────
 *   K1 ── PC0          K2 ── PC1
 *
 *   按键另一端接 GND，引脚在 CubeMX 里配成**上拉输入**，
 *   因此**按下时读到低电平**（GPIO_PIN_RESET）。
 *
 *   若实际接法相反（另一端接 VCC、按下读高），把 bsp_key.c 里的
 *   BSP_KEY_ACTIVE_LEVEL 改成 GPIO_PIN_SET，并把 .ioc 里这两个引脚
 *   改成下拉——两处必须同时改，否则消抖逻辑会一直认为键是按下状态。
 * ────────────────────────────────────────────────────────────────────
 *
 * ── 为什么消抖挂在 1 ms 节拍上，而不是主循环里 ───────────────────────
 *   机械触点在按下/松开的瞬间会弹跳几毫秒，直接读会得到一串忽高忽低的
 *   毛刺（一次按下被当成十几次）。消抖的标准做法是「定期采样 + 连续 N 次
 *   一致才认定电平真的变了」。
 *
 *   关键在**定期**：本工程刷新一屏 OLED 要在软件 I2C 上搬 1 KB 数据，
 *   耗时在几十毫秒量级。若靠在主循环里采样，采样间隔会随「这一轮有没有
 *   刷屏」剧烈变化——一次几十毫秒的短按可能整段落在刷屏期间，直接漏掉。
 *   挂到 1 ms 节拍上，采样间隔就与主循环在忙什么无关。
 *
 * ── 使用方式 ────────────────────────────────────────────────────────
 *   主循环里轮询事件；事件是「边沿」，读取即清除：
 *
 *     if (bsp_key_take_event(KEY_ID_K1) == KEY_EVENT_PRESS) { ... 按下瞬间 ... }
 *
 *   想知道「现在是不是按着」，用 bsp_key_is_pressed()（电平）。
 */
#ifndef BSP_KEY_H
#define BSP_KEY_H

#include <stdbool.h>
#include <stdint.h>

#include "interface/error.h"

/** 按键编号。 */
typedef enum {
    KEY_ID_K1 = 0,              /**< PC0 */
    KEY_ID_K2,                  /**< PC1 */
    KEY_ID_COUNT,               /**< 按键总数，也用作遍历上界 */
} key_id_t;

/** 按键事件（边沿）。 */
typedef enum {
    KEY_EVENT_NONE = 0,         /**< 自上次读取以来没有事件 */
    KEY_EVENT_PRESS,            /**< 按下 */
    KEY_EVENT_RELEASE,          /**< 松开 */
} key_event_t;

/**
 * @brief 初始化两个按键。
 *
 * 用当前实际电平初始化消抖状态（避免开机瞬间凭空产生一个按下事件）。
 * **不配置引脚**——引脚模式（输入 + 上拉）由 CubeMX 生成的 MX_GPIO_Init()
 * 完成，本函数只登记状态。
 *
 * @return error_t ERR_OK 成功。
 *
 * @note 必须在 MX_GPIO_Init() **之后**调用：引脚还没配好就读电平，
 *       读到的是浮空值，记录的初始状态就是错的。
 */
error_t bsp_key_init(void);

/**
 * @brief 采样一次两个按键并推进消抖状态机。
 *
 * 由 SysTick 中断每毫秒调用一次（见 Core/Src/stm32f4xx_it.c 的
 * USER CODE 块），**运行在中断上下文**。应用不需要也不应该主动调用它。
 */
void bsp_key_scan_isr(void);

/**
 * @brief 取走该键自上次取走以来发生的事件（读取即清除）。
 *
 * @param id 按键编号。
 *
 * @return key_event_t 发生的事件；没有则返回 KEY_EVENT_NONE。
 *
 * @note 同一个键的按下与松开若都还没被取走，会先返回 PRESS，
 *       下一次调用再返回 RELEASE，不会丢事件。
 */
key_event_t bsp_key_take_event(key_id_t id);

/**
 * @brief 该键当前是否处于按下状态（已消抖）。
 *
 * @param id 按键编号。
 *
 * @return bool true 表示稳定地处于按下状态。
 */
bool bsp_key_is_pressed(key_id_t id);

#endif /* BSP_KEY_H */
