/**
 * @file app_main.h
 * @brief 应用层入口。
 *
 * 当前内容是「工具链 → 板子 → 串口」整条链路的冒烟验证：开机打印横幅，
 * 周期性打印心跳，并把串口收到的字节原样回显。等链路确认可靠之后，
 * 温控风扇的状态机再替换掉 app_main_run() 里的这个循环。
 */
#ifndef APP_MAIN_H
#define APP_MAIN_H

#include "interface/error.h"

/**
 * @brief 应用初始化。在 main() 里、外设初始化完成之后调用一次。
 *
 * @return error_t ERR_OK 就绪；ERR_NOT_INITIALIZED 取不到串口契约。
 */
error_t app_main_init(void);

/**
 * @brief 应用主循环。正常情况**永不返回**。
 */
void app_main_run(void);

#endif /* APP_MAIN_H */
