#include "ssd1306.h"
#include "stm32f4xx_ll_i2c.h"
#include "stm32f4xx_ll_dma.h"
#include "stm32f4xx_ll_gpio.h"
#include "stm32f4xx_ll_bus.h"
#include "stm32f4xx_ll_utils.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include <string.h>

#define OLED_ADDR  0x78   /* 7-bit 0x3C << 1 */

/* ── Double buffers ──────────────────────────────────────────────────────── */
static uint8_t  buf_a[SSD1306_BUFFER_SIZE];
static uint8_t  buf_b[SSD1306_BUFFER_SIZE];
static uint8_t *front_buf = buf_a;   /* DMA reads this */
static uint8_t *back_buf  = buf_b;   /* task draws here */

/* s_done is given by ISR when DMA finishes, taken by UpdateScreen before swap.
 * Pre-armed with 1 in Init so the first UpdateScreen call never blocks. */
static SemaphoreHandle_t s_done = NULL;

/* ── I2C blocking primitives ─────────────────────────────────────────────── */

static void i2c_wait_free(void)
{
    uint32_t t = 200000;
    while (LL_I2C_IsActiveFlag_BUSY(I2C1) && --t) {}
}

static int i2c_begin(uint8_t ctrl)
{
    uint32_t t;

    LL_I2C_GenerateStartCondition(I2C1);
    t = 20000;
    while (!LL_I2C_IsActiveFlag_SB(I2C1) && --t) {}
    if (!t) { LL_I2C_GenerateStopCondition(I2C1); return 0; }

    LL_I2C_TransmitData8(I2C1, OLED_ADDR);
    t = 20000;
    while (!LL_I2C_IsActiveFlag_ADDR(I2C1) && --t) {
        if (LL_I2C_IsActiveFlag_AF(I2C1)) {
            LL_I2C_ClearFlag_AF(I2C1);
            LL_I2C_GenerateStopCondition(I2C1);
            return 0;
        }
    }
    if (!t) { LL_I2C_GenerateStopCondition(I2C1); return 0; }
    LL_I2C_ClearFlag_ADDR(I2C1);

    LL_I2C_TransmitData8(I2C1, ctrl);
    t = 20000;
    while (!LL_I2C_IsActiveFlag_TXE(I2C1) && --t) {}
    if (!t) { LL_I2C_GenerateStopCondition(I2C1); return 0; }

    return 1;
}

static int i2c_write(uint8_t byte)
{
    LL_I2C_TransmitData8(I2C1, byte);
    uint32_t t = 20000;
    while (!LL_I2C_IsActiveFlag_TXE(I2C1) && --t) {}
    if (!t) { LL_I2C_GenerateStopCondition(I2C1); return 0; }
    return 1;
}

static void i2c_end(void)
{
    uint32_t t = 20000;
    while (!LL_I2C_IsActiveFlag_BTF(I2C1) && --t) {}
    LL_I2C_GenerateStopCondition(I2C1);
}

static void i2c_send(const uint8_t *buf, size_t len)
{
    if (!len) return;
    i2c_wait_free();
    if (!i2c_begin(buf[0])) return;
    for (size_t i = 1; i < len; i++)
        if (!i2c_write(buf[i])) return;
    i2c_end();
}

/* ── Hardware init ───────────────────────────────────────────────────────── */

static void bus_recovery(void)
{
    LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_GPIOB);

    LL_GPIO_SetPinMode      (GPIOB, LL_GPIO_PIN_8, LL_GPIO_MODE_OUTPUT);
    LL_GPIO_SetPinOutputType(GPIOB, LL_GPIO_PIN_8, LL_GPIO_OUTPUT_OPENDRAIN);
    LL_GPIO_SetPinSpeed     (GPIOB, LL_GPIO_PIN_8, LL_GPIO_SPEED_FREQ_VERY_HIGH);
    LL_GPIO_SetPinPull      (GPIOB, LL_GPIO_PIN_8, LL_GPIO_PULL_UP);
    LL_GPIO_SetPinMode      (GPIOB, LL_GPIO_PIN_9, LL_GPIO_MODE_OUTPUT);
    LL_GPIO_SetPinOutputType(GPIOB, LL_GPIO_PIN_9, LL_GPIO_OUTPUT_OPENDRAIN);
    LL_GPIO_SetPinSpeed     (GPIOB, LL_GPIO_PIN_9, LL_GPIO_SPEED_FREQ_VERY_HIGH);
    LL_GPIO_SetPinPull      (GPIOB, LL_GPIO_PIN_9, LL_GPIO_PULL_UP);

    LL_GPIO_SetOutputPin(GPIOB, LL_GPIO_PIN_8);
    LL_GPIO_SetOutputPin(GPIOB, LL_GPIO_PIN_9);
    LL_mDelay(1);

    for (int i = 0; i < 9; i++) {
        if (LL_GPIO_IsInputPinSet(GPIOB, LL_GPIO_PIN_9)) break;
        LL_GPIO_ResetOutputPin(GPIOB, LL_GPIO_PIN_8); LL_mDelay(1);
        LL_GPIO_SetOutputPin  (GPIOB, LL_GPIO_PIN_8); LL_mDelay(1);
    }
    LL_GPIO_ResetOutputPin(GPIOB, LL_GPIO_PIN_9); LL_mDelay(1);
    LL_GPIO_ResetOutputPin(GPIOB, LL_GPIO_PIN_8); LL_mDelay(1);
    LL_GPIO_SetOutputPin  (GPIOB, LL_GPIO_PIN_8); LL_mDelay(1);
    LL_GPIO_SetOutputPin  (GPIOB, LL_GPIO_PIN_9); LL_mDelay(1);
}

static void i2c_hw_init(void)
{
    bus_recovery();

    LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_GPIOB);
    LL_GPIO_SetPinMode      (GPIOB, LL_GPIO_PIN_8, LL_GPIO_MODE_ALTERNATE);
    LL_GPIO_SetAFPin_8_15    (GPIOB, LL_GPIO_PIN_8, LL_GPIO_AF_4);
    LL_GPIO_SetPinSpeed     (GPIOB, LL_GPIO_PIN_8, LL_GPIO_SPEED_FREQ_VERY_HIGH);
    LL_GPIO_SetPinOutputType(GPIOB, LL_GPIO_PIN_8, LL_GPIO_OUTPUT_OPENDRAIN);
    LL_GPIO_SetPinPull      (GPIOB, LL_GPIO_PIN_8, LL_GPIO_PULL_UP);
    LL_GPIO_SetPinMode      (GPIOB, LL_GPIO_PIN_9, LL_GPIO_MODE_ALTERNATE);
    LL_GPIO_SetAFPin_8_15    (GPIOB, LL_GPIO_PIN_9, LL_GPIO_AF_4);
    LL_GPIO_SetPinSpeed     (GPIOB, LL_GPIO_PIN_9, LL_GPIO_SPEED_FREQ_VERY_HIGH);
    LL_GPIO_SetPinOutputType(GPIOB, LL_GPIO_PIN_9, LL_GPIO_OUTPUT_OPENDRAIN);
    LL_GPIO_SetPinPull      (GPIOB, LL_GPIO_PIN_9, LL_GPIO_PULL_UP);

    LL_APB1_GRP1_EnableClock  (LL_APB1_GRP1_PERIPH_I2C1);
    LL_APB1_GRP1_ForceReset   (LL_APB1_GRP1_PERIPH_I2C1);
    LL_APB1_GRP1_ReleaseReset (LL_APB1_GRP1_PERIPH_I2C1);
    LL_I2C_Disable(I2C1);

    LL_I2C_InitTypeDef cfg = {0};
    cfg.PeripheralMode  = LL_I2C_MODE_I2C;
    cfg.ClockSpeed      = 400000;
    cfg.DutyCycle       = LL_I2C_DUTYCYCLE_2;
    cfg.OwnAddress1     = 0;
    cfg.TypeAcknowledge = LL_I2C_ACK;
    cfg.OwnAddrSize     = LL_I2C_OWNADDRESS1_7BIT;
    LL_I2C_Init(I2C1, &cfg);
    LL_I2C_Enable(I2C1);
}

static void dma_hw_init(void)
{
    LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_DMA1);
    LL_DMA_DisableStream(DMA1, LL_DMA_STREAM_6);   /* I2C1_TX = DMA1 Stream6 Ch1 */

    LL_DMA_ConfigTransfer(DMA1, LL_DMA_STREAM_6,
        LL_DMA_DIRECTION_MEMORY_TO_PERIPH |
        LL_DMA_PRIORITY_HIGH              |
        LL_DMA_MODE_NORMAL                |
        LL_DMA_MDATAALIGN_BYTE            |
        LL_DMA_PDATAALIGN_BYTE            |
        LL_DMA_MEMORY_INCREMENT           |
        LL_DMA_PERIPH_NOINCREMENT);

    LL_DMA_SetChannelSelection(DMA1, LL_DMA_STREAM_6, LL_DMA_CHANNEL_1);
    LL_DMA_SetPeriphAddress   (DMA1, LL_DMA_STREAM_6, (uint32_t)&I2C1->DR);
    LL_DMA_EnableIT_TC        (DMA1, LL_DMA_STREAM_6);

    NVIC_SetPriority(DMA1_Stream6_IRQn, configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY);
    NVIC_EnableIRQ  (DMA1_Stream6_IRQn);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

uint8_t SSD1306_Init(void)
{
    i2c_hw_init();
    dma_hw_init();

    s_done = xSemaphoreCreateBinary();
    xSemaphoreGive(s_done);   /* pre-arm: first UpdateScreen won't block */

    LL_mDelay(150);

    static const uint8_t init_seq[] = {
        0x00,
        0xAE,         /* display off */
        0xD5, 0x80,   /* clock divide */
        0xA8, 0x3F,   /* multiplex 64 */
        0xD3, 0x00,   /* display offset 0 */
        0x40,         /* start line 0 */
        0x8D, 0x14,   /* charge pump on */
        0x20, 0x00,   /* horizontal addressing mode */
        0xA1,         /* seg remap */
        0xC8,         /* COM scan reversed */
        0xDA, 0x12,   /* COM pins */
        0x81, 0xCF,   /* contrast */
        0xD9, 0xF1,   /* pre-charge */
        0xDB, 0x40,   /* VCOMH */
        0xA4,         /* output from RAM */
        0xA6,         /* normal display */
        0xAF,         /* display on */
    };
    i2c_send(init_seq, sizeof(init_seq));

    memset(buf_a, 0, SSD1306_BUFFER_SIZE);
    memset(buf_b, 0, SSD1306_BUFFER_SIZE);
    front_buf = buf_a;
    back_buf  = buf_b;

    /* Initial blank frame — blocking I2C, no DMA (pre-scheduler) */
    static const uint8_t addr_win[] = {
        0x00, 0x21, 0x00, 0x7F,
              0x22, 0x00, 0x07,
    };
    i2c_send(addr_win, sizeof(addr_win));
    i2c_wait_free();
    if (i2c_begin(0x40)) {
        for (int i = 0; i < SSD1306_BUFFER_SIZE; i++)
            i2c_write(0);
        i2c_end();
    }

    return 1;
}

void SSD1306_Clear(void)
{
    memset(back_buf, 0, SSD1306_BUFFER_SIZE);
}

uint8_t SSD1306_IsBusy(void) { return 0; }

/* ── Double-buffer DMA update — non-blocking ─────────────────────────────
 *
 * Timeline per frame (33ms @ 30fps, DMA takes ~23ms):
 *
 *   t=0ms   UpdateScreen() called
 *             wait for prev DMA done  (≈0ms — already finished)
 *             swap buffers
 *             addr-window cmd         (~0.16ms blocking)
 *             start DMA               (returns immediately)
 *   t=0.3ms  returns → task draws next frame to back_buf
 *   t=23ms   DMA ISR fires → STOP → give semaphore
 *   t=33ms   vTaskDelay expires → task calls UpdateScreen() again
 *
 * Other tasks run freely during the 23ms DMA phase.
 * ───────────────────────────────────────────────────────────────────────── */
void SSD1306_UpdateScreen(void)
{
    /* Wait until previous frame's DMA has finished.
     * At 30fps this is usually instant (DMA done in 23ms, frame period 33ms). */
    xSemaphoreTake(s_done, pdMS_TO_TICKS(100));

    /* Swap: front_buf ← just-drawn back_buf (DMA will read it)
     *       back_buf  ← old front_buf (task can immediately draw next frame) */
    uint8_t *tmp = front_buf;
    front_buf    = back_buf;
    back_buf     = tmp;

    /* Address-window command — short blocking phase (~0.16ms) */
    static const uint8_t addr_win[] = {
        0x00, 0x21, 0x00, 0x7F,
              0x22, 0x00, 0x07,
    };
    i2c_send(addr_win, sizeof(addr_win));

    /* Open I2C data transaction: START → addr → 0x40 ctrl byte */
    i2c_wait_free();
    if (!i2c_begin(0x40)) {
        xSemaphoreGive(s_done);   /* re-arm so next call doesn't deadlock */
        return;
    }

    /* DMA takes over for the 1024 pixel bytes — function returns immediately */
    LL_DMA_ClearFlag_TC6(DMA1);
    LL_DMA_ClearFlag_TE6(DMA1);
    LL_DMA_SetMemoryAddress(DMA1, LL_DMA_STREAM_6, (uint32_t)front_buf);
    LL_DMA_SetDataLength   (DMA1, LL_DMA_STREAM_6, SSD1306_BUFFER_SIZE);
    LL_I2C_EnableDMAReq_TX (I2C1);
    LL_DMA_EnableStream    (DMA1, LL_DMA_STREAM_6);
    /* ← returns here; ISR will generate STOP and give semaphore */
}

/* ── DMA ISR ─────────────────────────────────────────────────────────────── */

void DMA1_Stream6_IRQHandler(void)
{
    if (!LL_DMA_IsActiveFlag_TC6(DMA1)) return;
    LL_DMA_ClearFlag_TC6(DMA1);

    /* Wait for shift register to finish clocking last byte (~22µs at 400kHz) */
    uint32_t t = 20000;
    while (!LL_I2C_IsActiveFlag_BTF(I2C1) && --t) {
        if (LL_I2C_IsActiveFlag_AF(I2C1)) { LL_I2C_ClearFlag_AF(I2C1); break; }
    }
    LL_I2C_GenerateStopCondition(I2C1);
    LL_I2C_DisableDMAReq_TX(I2C1);
    LL_DMA_DisableStream(DMA1, LL_DMA_STREAM_6);

    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_done, &woken);
    portYIELD_FROM_ISR(woken);
}

/* ── Draw primitives ─────────────────────────────────────────────────────── */

void SSD1306_DrawPixel(int16_t x, int16_t y, uint8_t color)
{
    if ((unsigned)x >= SSD1306_WIDTH || (unsigned)y >= SSD1306_HEIGHT) return;
    int idx = x + (y / 8) * SSD1306_WIDTH;
    if (color) back_buf[idx] |=  (1u << (y % 8));
    else        back_buf[idx] &= ~(1u << (y % 8));
}

void SSD1306_DrawChar(int16_t x, int16_t y, char ch, const FontDef *font, uint8_t color)
{
    if (ch < 32 || ch > 126) return;
    uint16_t ci = (uint16_t)(ch - 32) * (font->width - 1);
    for (uint8_t col = 0; col < font->width - 1; col++) {
        uint8_t col_data = font->data[ci + col];
        for (uint8_t row = 0; row < font->height; row++) {
            if ((col_data >> row) & 1)
                SSD1306_DrawPixel(x + col, y + row, color);
        }
    }
}

void SSD1306_DrawString(int16_t x, int16_t y, const char *str, const FontDef *font, uint8_t color)
{
    for (; *str; str++, x += font->width)
        SSD1306_DrawChar(x, y, *str, font, color);
}

void SSD1306_DrawString2x(int16_t x, int16_t y, const char *str, const FontDef *font, uint8_t color)
{
    for (; *str; str++, x += font->width * 2) {
        if (*str < 32 || *str > 126) continue;
        uint16_t ci = (uint16_t)(*str - 32) * (font->width - 1);
        for (uint8_t col = 0; col < font->width - 1; col++) {
            uint8_t col_data = font->data[ci + col];
            for (uint8_t row = 0; row < font->height; row++) {
                if ((col_data >> row) & 1) {
                    SSD1306_DrawPixel(x + col*2,     y + row*2,     color);
                    SSD1306_DrawPixel(x + col*2 + 1, y + row*2,     color);
                    SSD1306_DrawPixel(x + col*2,     y + row*2 + 1, color);
                    SSD1306_DrawPixel(x + col*2 + 1, y + row*2 + 1, color);
                }
            }
        }
    }
}

void SSD1306_FillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t color)
{
    for (int16_t py = y; py < y + h; py++)
        for (int16_t px = x; px < x + w; px++)
            SSD1306_DrawPixel(px, py, color);
}

void SSD1306_InvertRect(int16_t x, int16_t y, int16_t w, int16_t h)
{
    for (int16_t py = y; py < y + h && py < SSD1306_HEIGHT; py++) {
        int page = py / 8, bit = py % 8;
        for (int16_t px = x; px < x + w && px < SSD1306_WIDTH; px++)
            back_buf[page * SSD1306_WIDTH + px] ^= (1u << bit);
    }
}
