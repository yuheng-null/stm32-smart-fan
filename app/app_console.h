/**
 * @file app_console.h
 * @brief 串口控制台：接收命令、执行、回执；并周期发送遥测。
 *
 * 本层负责把 module/console 解析出来的**通用命令**翻译成
 * app_control 的语义动作（fan_cmd_t），再把结果回执给上位机。
 * console 模块不认识风扇、也不认识状态机——两边在 app 层对接。
 */
#ifndef APP_CONSOLE_H
#define APP_CONSOLE_H

#include "interface/error.h"
#include "module/telemetry/telemetry.h"

/**
 * @brief 绑定串口契约并复位解析器。
 *
 * ⚠️ 必须在 bsp_uart_init() 之后调用（否则 read() 永远返回 0）。
 *
 * @return error_t ERR_OK；ERR_NOT_INITIALIZED 取不到串口契约。
 */
error_t app_console_init(void);

/**
 * @brief 收命令。**每个主循环都要调**，串口接收的实时性靠它保证。
 */
void app_console_poll(void);

/**
 * @brief 发遥测（200 ms 节拍）。
 */
void app_console_task(void);

/**
 * @brief 发送一段文本（供启动横幅等处使用）。
 *
 * @return error_t 见 uart_if 的 write 约定。
 */
error_t app_console_send(const char *text, uint32_t len);

#endif /* APP_CONSOLE_H */
