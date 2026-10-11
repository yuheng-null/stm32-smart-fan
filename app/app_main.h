/**
 * @file app_main.h
 * @brief 应用层入口。
 *
 * 当前内容是「OLED + NTC + 按键」三个外设的**驱动测试台**：
 * 周期采样温度、上报按键事件，并把结果同时送到串口和 OLED 上。
 * 三者互相独立——OLED 没接或没应答时，串口上的温度与按键仍然正常，
 * 这样一次就能把「哪个器件的问题」分清楚。
 *
 * 等这三个驱动确认可用之后，温控风扇的状态机再替换掉 app_main_run()
 * 里的这个循环，业务逻辑最终落到 module/ 下。
 */
#ifndef APP_MAIN_H
#define APP_MAIN_H

#include "interface/error.h"

/**
 * @brief 应用初始化。在 main() 里、外设初始化完成之后调用一次。
 *
 * @return error_t ERR_OK 就绪；ERR_NOT_INITIALIZED 取不到串口契约。
 *
 * @note OLED 初始化失败**不会**让本函数失败：屏不亮时温度与按键的
 *       测试仍然有意义，故障经串口报出即可。
 */
error_t app_main_init(void);

/**
 * @brief 应用主循环。正常情况**永不返回**。
 */
void app_main_run(void);

#endif /* APP_MAIN_H */
