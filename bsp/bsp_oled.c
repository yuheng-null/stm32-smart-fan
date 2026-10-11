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

/* 字库：绘制接口要用。字体数据本身是可移植的（不含任何 STM32 符号），
   但「本板用哪几个字号」是板级决定，所以只在这里引。 */
#include "driver/device/ssd1306/ssd1306_fonts.h"

/* 屏宽必须与驱动配置同源：若改了 ssd1306_conf.h 的 SSD1306_WIDTH 而忘了改
   bsp_oled.h 的 BSP_OLED_WIDTH_PX，下面这行会编译不过（数组长度为负）。
   之所以要这道断言，是因为布局代码按 BSP_OLED_WIDTH_PX 算右对齐，
   两个值不一致时不会报错，只会把文字画到屏幕外。 */
typedef char bsp_oled_width_must_match_ssd1306_conf[
    (BSP_OLED_WIDTH_PX == SSD1306_WIDTH) ? 1 : -1];

/**
 * 本板唯一的屏幕实例。显存 1 KB，必须静态存储期。
 *
 * 收在本模块里、不导出类型，是为了让 app/ 那层完全不认识 ssd1306_t
 * （理由见 bsp_oled.h 顶部）。
 */
static ssd1306_t s_oled;

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

error_t bsp_oled_init(void)
{
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

    s_oled.bus      = bus;
    s_oled.addr     = s_addr;
    s_oled.delay_ms = bsp_oled_delay_ms;

    return ssd1306_init(&s_oled);
}

uint8_t bsp_oled_get_addr(void)
{
    return s_addr;
}

/* ------------------------------------------------------------- 绘制 */

void bsp_oled_clear(void)
{
    (void)ssd1306_fill(&s_oled, SSD1306_BLACK);
}

void bsp_oled_draw_text(uint16_t x, uint16_t y, const char *text, bsp_oled_font_t font)
{
    if (text == NULL) {
        return;
    }

    const SSD1306_Font_t *glyphs = (font == BSP_OLED_FONT_LARGE) ? &Font_11x18
                                                                 : &Font_6x8;

    (void)ssd1306_set_cursor(&s_oled, x, y);
    (void)ssd1306_write_string(&s_oled, text, glyphs, SSD1306_WHITE);
}

uint16_t bsp_oled_text_width(const char *text, bsp_oled_font_t font)
{
    if (text == NULL) {
        return 0U;
    }

    /* 这两个步进是字库的固有属性：6×8 与 11×18 的字符格宽。
       写成常量而不是从字体结构体里推，是因为 ssd1306_write_string 本身就是
       按「格宽」推进游标的，两边必须一致。 */
    const uint16_t step = (font == BSP_OLED_FONT_LARGE) ? 11U : 6U;
    uint16_t       n    = 0U;

    while (text[n] != '\0') {
        n++;
    }

    return (uint16_t)(n * step);
}

void bsp_oled_flush(void)
{
    (void)ssd1306_update_screen(&s_oled);
}
