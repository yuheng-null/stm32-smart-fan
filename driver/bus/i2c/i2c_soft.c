/**
 * @file    i2c_soft.c
 * @brief   软件模拟 I2C 主机总线实现（STM32F4 + HAL）。
 *
 * 时序约定（I2C 标准）：
 *   · 起始条件：SCL 为高时，SDA 由高变低
 *   · 停止条件：SCL 为高时，SDA 由低变高
 *   · 数据位  ：SCL 为低时改变 SDA，SCL 为高时 SDA 必须稳定
 *   · 应答位  ：发送方在第 9 个时钟释放 SDA，接收方拉低表示应答（ACK）
 *
 * 关于「开漏输出还能读回电平」：
 *   RM0090 §8.3.10（Output configuration）写明：端口配成输出时
 *   「the Schmitt trigger input is activated」「the data present on the I/O pin
 *   are sampled into the input data register every AHB1 clock cycle」，
 *   且「a read access to the input data register gets the I/O state」。
 *   因此把 SDA 配成开漏输出后，可以直接用 HAL_GPIO_ReadPin 读回线路真实电平
 *   （它读的是 IDR，不是 ODR），不必在输入/输出模式之间来回切换。
 *   这正是位翻转 I2C 不需要切模式的原因。
 *
 *   ⚠️ 注意这里读的是**引脚**电平而非写入值——开漏输出写 1 时引脚是 Hi-Z，
 *      只有外部上拉或从机拉低才决定它的实际电平，ACK 检测正是靠这一点。
 */

#include "i2c_soft.h"

/* ----------------------------------------------------------------- 时序 */

/**
 * 退化路径下估算延时用的循环次数（每微秒）。
 *
 * 仅在 DWT 周期计数器不可用时使用。它的唯一要求是「宁可偏慢，不可偏快」：
 * 延时偏长只会让总线变慢（功能正确），偏短则可能超出从机能力而通信失败。
 *
 * ⚠️ 这个值是**估算，未经实测**。本工程主频 168 MHz，循环体是
 *   volatile 自增 + 比较 + 跳转，按「每次迭代约 4 个周期」粗估约 42 次/µs；
 *   这里取 60，方向偏慢，留了约 40% 余量。
 *
 *   之所以允许它只是估算：正常路径走 DWT，这条退化路径只在
 *   调试器占用了 DWT 计数器之类的异常情况下才会被走到。
 *   真要长期依赖它，必须像 README 里验时钟那样**实测**一遍
 *   （例如在示波器/逻辑分析仪上量 SCL 周期），不能沿用这里的数字。
 */
#define I2C_SOFT_FALLBACK_LOOPS_PER_US  (60U)

/** 1 秒 = 1000000 微秒 */
#define I2C_SOFT_US_PER_SEC             (1000000U)

/** DWT 周期计数器是否可用，见 i2c_soft_timing_init() */
static bool s_dwt_available = false;

/**
 * @brief 使能并自检 DWT 周期计数器。
 *
 * DWT->CYCCNT 是最方便的高精度延时源，但个别情况下（core debug 被关闭等）
 * 它不会计数，若直接死等会卡死，因此这里先验证它确实在走。
 */
static void i2c_soft_timing_init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    const uint32_t start = DWT->CYCCNT;
    for (volatile uint32_t i = 0U; i < 64U; i++) {
        /* 空转一小会儿，给计数器时间 */
    }
    s_dwt_available = (DWT->CYCCNT != start);
}

/**
 * @brief 半周期延时。
 */
static void i2c_soft_delay(const i2c_soft_t *bus)
{
    if (s_dwt_available) {
        const uint32_t cycles_per_us = SystemCoreClock / I2C_SOFT_US_PER_SEC;
        const uint32_t wait_cycles    = bus->delay_us * cycles_per_us;
        const uint32_t start          = DWT->CYCCNT;

        while ((DWT->CYCCNT - start) < wait_cycles) {
            /* 等待 */
        }
    } else {
        const uint32_t loops = bus->delay_us * I2C_SOFT_FALLBACK_LOOPS_PER_US;

        for (volatile uint32_t i = 0U; i < loops; i++) {
            /* 等待 */
        }
    }
}

/* ------------------------------------------------------------- 引脚操作 */

static void i2c_soft_scl_high(const i2c_soft_t *bus)
{
    HAL_GPIO_WritePin(bus->cfg.scl_port, bus->cfg.scl_pin, GPIO_PIN_SET);
}

static void i2c_soft_scl_low(const i2c_soft_t *bus)
{
    HAL_GPIO_WritePin(bus->cfg.scl_port, bus->cfg.scl_pin, GPIO_PIN_RESET);
}

static void i2c_soft_sda_high(const i2c_soft_t *bus)
{
    HAL_GPIO_WritePin(bus->cfg.sda_port, bus->cfg.sda_pin, GPIO_PIN_SET);
}

static void i2c_soft_sda_low(const i2c_soft_t *bus)
{
    HAL_GPIO_WritePin(bus->cfg.sda_port, bus->cfg.sda_pin, GPIO_PIN_RESET);
}

/** 读回 SDA 实际电平（开漏输出模式下读 IDR 即可，见文件头说明） */
static bool i2c_soft_sda_is_high(const i2c_soft_t *bus)
{
    return HAL_GPIO_ReadPin(bus->cfg.sda_port, bus->cfg.sda_pin) == GPIO_PIN_SET;
}

/**
 * @brief 把 SCL/SDA 配成开漏输出。
 *
 * 开漏是 I2C 的硬性要求：任何一方都只能把线拉低，不能主动拉高，
 * 否则会出现两个器件同时输出相反电平的短路。高电平靠上拉电阻提供。
 *
 * ── 关于 gpio.Pull = GPIO_PULLUP ────────────────────────────────────
 * 在 STM32F4 上这句**真的生效**：F4 的 GPIO 有独立的上下拉寄存器 PUPDR
 * （RM0090 §8.3.2 的表里，输出模式同样可以选择 pull-up/pull-down），
 * 所以开漏输出时内部约 40 kΩ 的上拉会把线拉高。
 *
 *   ⚠️ 但这**不能**当成总线的主要上拉来源：40 kΩ 配几十 pF 的总线电容，
 *      上升时间在微秒量级，400 kHz 下会明显吃紧。
 *      本板实际依赖的是 OLED 模块**板载**的 4.7 kΩ 上拉——见
 *      bsp/bsp_oled.c 顶部的接线说明。
 *      内部上拉的作用是**兜底**：万一线断了或模块没插，总线不会浮空，
 *      ACK 检测得到的是稳定的「无应答」而不是随机值。
 * ────────────────────────────────────────────────────────────────────
 */
static void i2c_soft_gpio_config(GPIO_TypeDef *port, uint16_t pin)
{
    GPIO_InitTypeDef gpio = {0};

    HAL_GPIO_WritePin(port, pin, GPIO_PIN_SET); /* 输出寄存器置高 = 释放总线 */

    gpio.Pin   = pin;
    gpio.Mode  = GPIO_MODE_OUTPUT_OD;
    gpio.Pull  = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;

    HAL_GPIO_Init(port, &gpio);
}

/* --------------------------------------------------------------- 协议层 */

static void i2c_soft_start(const i2c_soft_t *bus)
{
    i2c_soft_sda_high(bus);
    i2c_soft_scl_high(bus);
    i2c_soft_delay(bus);

    i2c_soft_sda_low(bus); /* SCL 为高时 SDA 下降沿 = 起始条件 */
    i2c_soft_delay(bus);

    i2c_soft_scl_low(bus);
    i2c_soft_delay(bus);
}

static void i2c_soft_stop(const i2c_soft_t *bus)
{
    i2c_soft_sda_low(bus);
    i2c_soft_scl_high(bus);
    i2c_soft_delay(bus);

    i2c_soft_sda_high(bus); /* SCL 为高时 SDA 上升沿 = 停止条件 */
    i2c_soft_delay(bus);
}

/**
 * @brief 发送一个字节，并读回从机应答。
 *
 * @return true 从机应答（ACK）；false 无应答（NACK，通常意味着器件不在或地址错）
 */
static bool i2c_soft_write_byte(const i2c_soft_t *bus, uint8_t byte)
{
    for (uint8_t mask = 0x80U; mask != 0U; mask >>= 1) {
        if ((byte & mask) != 0U) {
            i2c_soft_sda_high(bus);
        } else {
            i2c_soft_sda_low(bus);
        }
        i2c_soft_delay(bus);

        i2c_soft_scl_high(bus); /* SCL 拉高，从机在此刻采样 SDA */
        i2c_soft_delay(bus);

        i2c_soft_scl_low(bus);
        i2c_soft_delay(bus);
    }

    /* 第 9 个时钟：主机释放 SDA，由从机决定电平 */
    i2c_soft_sda_high(bus);
    i2c_soft_delay(bus);

    i2c_soft_scl_high(bus);
    i2c_soft_delay(bus);

    const bool acked = !i2c_soft_sda_is_high(bus);

    i2c_soft_scl_low(bus);
    i2c_soft_delay(bus);

    return acked;
}

/**
 * @brief 接收一个字节，并由主机回一个应答位。
 *
 * @param ack true 回 ACK（还要继续读）；false 回 NACK（最后一个字节）
 */
static uint8_t i2c_soft_read_byte(const i2c_soft_t *bus, bool ack)
{
    uint8_t byte = 0U;

    i2c_soft_sda_high(bus); /* 释放 SDA，交给从机驱动 */
    i2c_soft_delay(bus);

    for (uint8_t i = 0U; i < 8U; i++) {
        byte <<= 1;

        i2c_soft_scl_high(bus); /* SCL 高电平期间数据有效，采样 */
        i2c_soft_delay(bus);

        if (i2c_soft_sda_is_high(bus)) {
            byte |= 0x01U;
        }

        i2c_soft_scl_low(bus);
        i2c_soft_delay(bus);
    }

    /* 第 9 个时钟：主机输出应答位 */
    if (ack) {
        i2c_soft_sda_low(bus);  /* ACK */
    } else {
        i2c_soft_sda_high(bus); /* NACK */
    }
    i2c_soft_delay(bus);

    i2c_soft_scl_high(bus);
    i2c_soft_delay(bus);

    i2c_soft_scl_low(bus);
    i2c_soft_delay(bus);

    i2c_soft_sda_high(bus); /* 释放 SDA */
    i2c_soft_delay(bus);

    return byte;
}

/**
 * @brief 发送从机地址 + 读写位。
 *
 * @param read true 表示读方向（地址末位为 1）
 */
static bool i2c_soft_send_addr(const i2c_soft_t *bus, uint8_t addr, bool read)
{
    const uint8_t frame = (uint8_t)((addr << 1) | (read ? 0x01U : 0x00U));

    return i2c_soft_write_byte(bus, frame);
}

/* ------------------------------------------------------- i2c_if_t 实现 */

static error_t i2c_soft_if_write(void *ctx, uint8_t addr, const uint8_t *data, uint32_t len)
{
    i2c_soft_t *bus = (i2c_soft_t *)ctx;

    if ((bus == NULL) || (!bus->ready)) {
        return ERR_NOT_INITIALIZED;
    }
    if ((data == NULL) || (len == 0U)) {
        return ERR_INVALID_PARAM;
    }

    i2c_soft_start(bus);

    if (!i2c_soft_send_addr(bus, addr, false)) {
        i2c_soft_stop(bus);
        return ERR_COMMUNICATION;
    }

    for (uint32_t i = 0U; i < len; i++) {
        if (!i2c_soft_write_byte(bus, data[i])) {
            i2c_soft_stop(bus);
            return ERR_COMMUNICATION;
        }
    }

    i2c_soft_stop(bus);

    return ERR_OK;
}

static error_t i2c_soft_if_read(void *ctx, uint8_t addr, uint8_t *data, uint32_t len)
{
    i2c_soft_t *bus = (i2c_soft_t *)ctx;

    if ((bus == NULL) || (!bus->ready)) {
        return ERR_NOT_INITIALIZED;
    }
    if ((data == NULL) || (len == 0U)) {
        return ERR_INVALID_PARAM;
    }

    i2c_soft_start(bus);

    if (!i2c_soft_send_addr(bus, addr, true)) {
        i2c_soft_stop(bus);
        return ERR_COMMUNICATION;
    }

    for (uint32_t i = 0U; i < len; i++) {
        /* 最后一个字节回 NACK，告诉从机「我不要了」，随后发停止条件 */
        data[i] = i2c_soft_read_byte(bus, (i + 1U) < len);
    }

    i2c_soft_stop(bus);

    return ERR_OK;
}

static error_t i2c_soft_if_write_read(void *ctx, uint8_t addr,
                                      const uint8_t *cmd, uint32_t cmd_len,
                                      uint8_t *rx, uint32_t rx_len)
{
    i2c_soft_t *bus = (i2c_soft_t *)ctx;

    if ((bus == NULL) || (!bus->ready)) {
        return ERR_NOT_INITIALIZED;
    }
    if ((cmd == NULL) || (cmd_len == 0U) || (rx == NULL) || (rx_len == 0U)) {
        return ERR_INVALID_PARAM;
    }

    i2c_soft_start(bus);

    if (!i2c_soft_send_addr(bus, addr, false)) {
        i2c_soft_stop(bus);
        return ERR_COMMUNICATION;
    }

    for (uint32_t i = 0U; i < cmd_len; i++) {
        if (!i2c_soft_write_byte(bus, cmd[i])) {
            i2c_soft_stop(bus);
            return ERR_COMMUNICATION;
        }
    }

    /* 重复起始条件：不发停止条件，直接重新起始并切到读方向 */
    i2c_soft_start(bus);

    if (!i2c_soft_send_addr(bus, addr, true)) {
        i2c_soft_stop(bus);
        return ERR_COMMUNICATION;
    }

    for (uint32_t i = 0U; i < rx_len; i++) {
        rx[i] = i2c_soft_read_byte(bus, (i + 1U) < rx_len);
    }

    i2c_soft_stop(bus);

    return ERR_OK;
}

/* ------------------------------------------------------------ 对外接口 */

error_t i2c_soft_init(i2c_soft_t *bus, const i2c_soft_cfg_t *cfg)
{
    if ((bus == NULL) || (cfg == NULL)) {
        return ERR_INVALID_PARAM;
    }
    if ((cfg->scl_port == NULL) || (cfg->sda_port == NULL)) {
        return ERR_INVALID_PARAM;
    }

    bus->cfg   = *cfg;
    bus->ready = false;

    /* 半周期延时：freq 是完整周期频率，故乘以 2；结果至少 1 微秒 */
    const uint32_t freq_hz = (cfg->freq_hz != 0U) ? cfg->freq_hz : I2C_SOFT_DEFAULT_FREQ_HZ;
    uint32_t       half_us = I2C_SOFT_US_PER_SEC / (freq_hz * 2U);
    if (half_us == 0U) {
        half_us = 1U;
    }
    bus->delay_us = half_us;

    bus->iface.write      = i2c_soft_if_write;
    bus->iface.read       = i2c_soft_if_read;
    bus->iface.write_read = i2c_soft_if_write_read;
    bus->iface.ctx        = bus;

    i2c_soft_timing_init();
    i2c_soft_gpio_config(cfg->scl_port, cfg->scl_pin);
    i2c_soft_gpio_config(cfg->sda_port, cfg->sda_pin);

    bus->ready = true;

    return ERR_OK;
}

i2c_if_t *i2c_soft_iface(i2c_soft_t *bus)
{
    if ((bus == NULL) || (!bus->ready)) {
        return NULL;
    }

    return &bus->iface;
}

error_t i2c_soft_bus_recover(i2c_soft_t *bus)
{
    if ((bus == NULL) || (!bus->ready)) {
        return ERR_NOT_INITIALIZED;
    }

    i2c_soft_sda_high(bus); /* 释放 SDA，让从机有机会放手 */

    for (uint8_t i = 0U; i < I2C_SOFT_RECOVER_CLOCKS; i++) {
        i2c_soft_scl_low(bus);
        i2c_soft_delay(bus);
        i2c_soft_scl_high(bus);
        i2c_soft_delay(bus);
    }

    i2c_soft_stop(bus);

    return i2c_soft_sda_is_high(bus) ? ERR_OK : ERR_BUSY;
}
