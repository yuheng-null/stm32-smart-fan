/**
 * @file console.h
 * @brief 串口行命令解析器：字节流 → 结构化命令。
 *
 * 纯计算，不含 HAL、不碰 uart_if。app 负责把串口收到的字节一个一个喂进
 * console_parser_feed()，本模块负责把它拼成「一整条命令」。
 *
 * ── 为什么不用 sscanf / strtod ───────────────────────────────────────
 *   ① newlib-nano 默认不带浮点格式化，`%f` 那套在目标机上是**链接期**才会
 *      暴露的坑；自己解析十进制就不依赖这些开关。
 *   ② 自己解析能把「越界」「溢出」「缺参」这些情况**显式**变成返回码，
 *      而不是让 sscanf 悄悄少填一个参数。
 *
 * ── 协议 ─────────────────────────────────────────────────────────────
 *   行协议：一行一条命令，`\r`、`\n`、`\r\n` 都算行结束；大小写不敏感；
 *   `?` 与命令词之间、命令词与参数之间用空白分隔。
 *   空行不产生命令（返回 ERR_NOT_READY），所以 `\r\n` 不会被当成两条。
 * ────────────────────────────────────────────────────────────────────
 */
#ifndef MODULE_CONSOLE_H
#define MODULE_CONSOLE_H

#include <stdbool.h>
#include <stdint.h>

#include "interface/error.h"

/** 一行最长字节数（含结尾 NUL 的余量）。超出的部分会被丢弃到行尾。 */
#define CONSOLE_LINE_MAX (48U)

/** 最多几个数字参数（PID 需要两个）。 */
#define CONSOLE_MAX_ARGS (2U)

/** 命令词的最大长度（含 NUL）。 */
#define CONSOLE_VERB_MAX (8U)

/** 解析出的命令。 */
/**
 * IR 命令的子动作，编码在 console_command_t::arg_i[0]。
 *
 * 放在头文件而不是藏在 .c 里：app 那一层要照着它分发，两边写各自的字面量
 * 迟早会漂移（`MODE` 的 0/1/2 就是现成的例子，已在 app_console.c 里重写了一遍）。
 */
typedef enum {
    CONSOLE_IR_STATUS    = 0,   /**< IR：打印遥控接收状态 */
    CONSOLE_IR_LEARN_OFF = 1,   /**< IR LEARN 0：关学习模式 */
    CONSOLE_IR_LEARN_ON  = 2,   /**< IR LEARN 1：开学习模式 */
} console_ir_action_t;

typedef enum {
    CONSOLE_CMD_NONE = 0,
    CONSOLE_CMD_HELP,      /**< ? */
    CONSOLE_CMD_STATUS,    /**< STATUS */
    CONSOLE_CMD_MODE,      /**< MODE OFF|AUTO|MANUAL */
    CONSOLE_CMD_MANUAL,    /**< MANUAL <0..100> */
    CONSOLE_CMD_TEMP,      /**< TEMP <摄氏度>，如 32.5 → arg_i[0] = 32500（毫摄氏度） */
    CONSOLE_CMD_PID,       /**< PID <kp> <ki>，两参数进 arg_f */
    CONSOLE_CMD_STREAM,    /**< STREAM <0|1>：0=人类可读行，1=CSV 遥测帧 */
    CONSOLE_CMD_RESET,     /**< RESET */
    CONSOLE_CMD_IR,        /**< IR、IR LEARN <0|1>；子动作编码在 arg_i[0] */
    CONSOLE_CMD_UNKNOWN,   /**< 认不出来或参数非法 */
} console_cmd_id_t;

/** 解析结果。 */
typedef struct {
    console_cmd_id_t id;
    int32_t          arg_i[CONSOLE_MAX_ARGS];   /**< 整数参数（TEMP 为毫摄氏度） */
    float            arg_f[CONSOLE_MAX_ARGS];   /**< 浮点参数（PID 增益） */
    uint8_t          arg_count;
    char             verb[CONSOLE_VERB_MAX];    /**< 原命令词（已转大写），回显用 */
} console_command_t;

/** 解析器状态（一行缓冲）。调用方持有实例，不要碰内部字段。 */
typedef struct {
    char    line[CONSOLE_LINE_MAX];
    uint8_t len;
    bool    overflow;   /**< 本行已溢出，字符丢弃到行尾为止 */
} console_parser_t;

/**
 * @brief 清空解析器（例如串口重连、丢弃半行垃圾）。
 *
 * @param parser 解析器；为空时空操作。
 */
void console_parser_reset(console_parser_t *parser);

/**
 * @brief 喂一个字节。
 *
 * @param parser 解析器。
 * @param ch     字节。
 * @param out    解析结果；**仅当返回 ERR_OK 时有意义**。
 *
 * @return error_t ERR_OK            解析出一条完整命令（可能是 UNKNOWN）；
 *                 ERR_NOT_READY     行还没结束，或者这是一行空行，继续喂；
 *                 ERR_INVALID_PARAM 行溢出或参数非法（id 为 UNKNOWN）。
 */
error_t console_parser_feed(console_parser_t *parser, uint8_t ch, console_command_t *out);

/**
 * @brief 帮助文本（静态字面量）。
 *
 * 返回的是常量字符串，app 直接发出去即可——不复制、不占 RAM。
 *
 * @return const char* 帮助文本（以 \r\n 结尾）。
 */
const char *console_help_text(void);

#endif /* MODULE_CONSOLE_H */
