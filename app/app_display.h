/**
 * @file app_display.h
 * @brief OLED 显示：把状态画到屏上。
 *
 * 视图内容由 module/display_view 决定（那里可在 PC 上单测行宽），
 * 本层只负责「怎么画」与「什么时候画」。
 */
#ifndef APP_DISPLAY_H
#define APP_DISPLAY_H

#include <stdbool.h>

#include "interface/error.h"

/**
 * @brief 初始化屏幕（内含 I2C 总线探测）。
 *
 * ⚠️ 必须在 bsp_i2c_init() 之后调用。
 *
 * @return error_t ERR_OK 屏幕可用；
 *                 ERR_COMMUNICATION 探测不到器件（**不算致命**，
 *                 串口与控制照常工作，只是没有显示）。
 */
error_t app_display_init(void);

/**
 * @brief 显示刷新任务（200 ms 节拍）。
 *
 * 内容与上一帧相同则**跳过**上屏：软件 I2C 搬 1 KB 要几十毫秒，
 * 而稳态下绝大多数周期内容是不变的，这部分开销可以直接省掉。
 */
void app_display_task(void);

/**
 * @brief 屏幕当前是否可用。
 *
 * @return bool true 表示屏幕已初始化成功。
 */
bool app_display_is_present(void);

#endif /* APP_DISPLAY_H */
