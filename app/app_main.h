/**
 * @file app_main.h
 * @brief 应用层入口：温控风扇的启动编排与主循环。
 *
 * `app_main_init()` 负责把 BSP 与各 app 模块按正确的先后顺序拉起来
 * （顺序有实际约束，见实现里的注释），`app_main_run()` 是一个
 * **时间触发**的协作式调度器：每个任务有自己的周期，互不阻塞。
 *
 * 业务逻辑不在这一层——它在 module/ 下，且是纯计算、可在 PC 上单测。
 * 本层只做「什么时候调用谁」。
 */
#ifndef APP_MAIN_H
#define APP_MAIN_H

#include "interface/error.h"

/**
 * @brief 应用初始化。在 main() 里调用一次。
 *
 * @return error_t ERR_OK 至少串口与控制链就绪；
 *                 ERR_NOT_INITIALIZED 取不到串口契约（无法运行）。
 *
 * @note 屏幕初始化失败**不会**让本函数失败：屏不亮时温控、遥测、按键
 *       全都照常，故障经串口报出来即可。
 */
error_t app_main_init(void);

/**
 * @brief 应用主循环。正常情况**永不返回**。
 */
void app_main_run(void);

#endif /* APP_MAIN_H */
