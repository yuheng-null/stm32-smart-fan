/**
 * @file bsp_oled.c
 * @brief 板级 OLED 实现（本板挂载总线：PB6=SCL / PB7=SDA）。
 */

#include "bsp/bsp_oled.h"

#include <stdbool.h>
#include <stddef.h>

#include "main.h"                   /* 借它引入 stm32f4xx_hal.h（HAL_Delay） */

#include "bsp/bsp_i2c.h"

/* 探测时要用控制字节 SSD1306_CTRL_BYTE_COMMAND；ssd1306.h 并不导出它，
   命令字集中在 ssd1306_reg.h 里，所以单独引一次。 */
#include "driver/device/ssd1306/ssd1306_reg.h"

/**
 * 候选从机地址，按优先级排列。
 *
 * 0.96 寸 SSD1306 模块只有两种地址，由板上 SA0 引脚决定：
 *   SA0 接地 → 0x3C       SA0 接 VCC → 0x3D
 * 绝大多数模块出厂就是 0x3C，先试它。
 */
static const uint8_t s_candidate_addr[] = { SSD1306_I2C_ADDR_7BIT, 0x3DU };

/** 实际用上的地址，0 表示尚未初始化。 */
static uint8_t s_addr = 0U;

/**
 * @brief 毫秒延时回调，注入给 SSD1306 驱动。
 *
 * 驱动层不许直接调 HAL_Delay（那会把它绑死在某平台上），
 * 所以由 BSP 这一层把 HAL_Delay 作为函数指针传进去。
 */
static void bsp_oled_delay_ms(uint32_t ms)
{
    HAL_Delay(ms);
}

/**
 * @brief 探测某地址上有没有器件应答。
 *
 * 手段：只发「地址 + 写方向」和一个控制字节，然后发停止条件。
 *
 *   控制字节 0x00 在 SSD1306 的 I2C 帧里表示「后面跟的是命令」，
 *   而后面**一个字节都没有**，所以这次传输对屏幕没有任何副作用
 *   ——它只是被寻址、回一个 ACK，然后看到停止条件。
 *
 *   为什么不沿用「读一个字节」的探测法：SSD1306 的 I2C 接口在多数
 *   模块上是只写的，读方向的地址阶段是否应答并不统一。写方向则所有
 *   模块都必须应答（不应答就没法用了），拿它作判据更可靠。
 *
 * @return true 该地址有人应答。
 */
static bool bsp_oled_probe(uint8_t addr)
{
    static const uint8_t probe_frame = SSD1306_CTRL_BYTE_COMMAND;
    i2c_if_t            *bus         = bsp_i2c_get();

    if (bus == NULL) {
        return false;
    }

    /*
     * 不应答不是故障，是「这个地址上没有器件」这一正常结论，
     * 所以这里只把结果转成 bool，不做任何恢复动作。
     */
    return (bus->write(bus->ctx, addr, &probe_frame, 1U) == ERR_OK);
}

uint8_t bsp_oled_bus_scan(uint8_t *found, uint8_t max)
{
    i2c_if_t *bus = bsp_i2c_get();
    uint8_t   count = 0U;

    if ((found == NULL) || (max == 0U) || (bus == NULL)) {
        return 0U;
    }

    /* 0x00~0x07 与 0x78~0x7F 是 I2C 保留地址，扫它们没有意义。 */
    for (uint8_t addr = 0x08U; addr <= 0x77U; addr++) {
        if (bsp_oled_probe(addr)) {
            if (count < max) {
                found[count] = addr;
            }
            count++;
        }
    }

    return count;
}

error_t bsp_oled_init(ssd1306_t *dev)
{
    if (dev == NULL) {
        return ERR_INVALID_PARAM;
    }

    i2c_if_t *bus = bsp_i2c_get();
    if (bus == NULL) {
        return ERR_NOT_INITIALIZED;
    }

    s_addr = 0U;

    for (size_t i = 0U; i < (sizeof(s_candidate_addr) / sizeof(s_candidate_addr[0])); i++) {
        if (bsp_oled_probe(s_candidate_addr[i])) {
            s_addr = s_candidate_addr[i];
            break;
        }
    }

    if (s_addr == 0U) {
        return ERR_COMMUNICATION;
    }

    dev->bus      = bus;
    dev->addr     = s_addr;
    dev->delay_ms = bsp_oled_delay_ms;

    return ssd1306_init(dev);
}

uint8_t bsp_oled_get_addr(void)
{
    return s_addr;
}
