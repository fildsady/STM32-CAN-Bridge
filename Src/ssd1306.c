/*
 * ssd1306.c — SSD1306 128x64 OLED driver (I2C1 + DMA, double buffer)
 *
 * STM32F446RE: I2C1 PB6(SCL) / PB7(SDA), DMA1 Stream6 Channel1
 * Same architecture as Pico version: draw to back_buf, swap+DMA on update()
 */
#include "ssd1306.h"
#include "stm32f4xx_hal.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include <string.h>

/* ── Double buffers ──────────────────────────────────────────────────── */
static uint8_t  buf_a[SSD1306_BUF_SIZE];
static uint8_t  buf_b[SSD1306_BUF_SIZE];
static uint8_t *front_buf = buf_a;
static uint8_t *back_buf  = buf_b;

/* DMA transfer buffer: 0x40 ctrl byte + 1024 pixel bytes */
static uint8_t s_dma_buf[1 + SSD1306_BUF_SIZE];

static I2C_HandleTypeDef s_hi2c;
static DMA_HandleTypeDef s_hdma_tx;
static SemaphoreHandle_t s_done = NULL;
static volatile bool     s_busy = false;

/* ── I2C1 + DMA init ────────────────────────────────────────────────── */
static void i2c_hw_init(void) {
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_I2C1_CLK_ENABLE();
    __HAL_RCC_DMA1_CLK_ENABLE();

    /* PB6 = SCL, PB7 = SDA → AF4 (I2C1) */
    GPIO_InitTypeDef gpio = {0};
    gpio.Pin = GPIO_PIN_6 | GPIO_PIN_7;
    gpio.Mode = GPIO_MODE_AF_OD;
    gpio.Pull = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    gpio.Alternate = GPIO_AF4_I2C1;
    HAL_GPIO_Init(GPIOB, &gpio);

    /* DMA1 Stream6 Channel1 = I2C1_TX */
    s_hdma_tx.Instance = DMA1_Stream6;
    s_hdma_tx.Init.Channel = DMA_CHANNEL_1;
    s_hdma_tx.Init.Direction = DMA_MEMORY_TO_PERIPH;
    s_hdma_tx.Init.PeriphInc = DMA_PINC_DISABLE;
    s_hdma_tx.Init.MemInc = DMA_MINC_ENABLE;
    s_hdma_tx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
    s_hdma_tx.Init.MemDataAlignment = DMA_MDATAALIGN_BYTE;
    s_hdma_tx.Init.Mode = DMA_NORMAL;
    s_hdma_tx.Init.Priority = DMA_PRIORITY_LOW;
    s_hdma_tx.Init.FIFOMode = DMA_FIFOMODE_DISABLE;
    HAL_DMA_Init(&s_hdma_tx);
    __HAL_LINKDMA(&s_hi2c, hdmatx, s_hdma_tx);

    HAL_NVIC_SetPriority(DMA1_Stream6_IRQn, 7, 0);
    HAL_NVIC_EnableIRQ(DMA1_Stream6_IRQn);

    /* I2C1: 400kHz fast mode */
    s_hi2c.Instance = I2C1;
    s_hi2c.Init.ClockSpeed = 400000;
    s_hi2c.Init.DutyCycle = I2C_DUTYCYCLE_2;
    s_hi2c.Init.OwnAddress1 = 0;
    s_hi2c.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
    s_hi2c.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
    s_hi2c.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
    s_hi2c.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
    HAL_I2C_Init(&s_hi2c);

    HAL_NVIC_SetPriority(I2C1_EV_IRQn, 7, 0);
    HAL_NVIC_EnableIRQ(I2C1_EV_IRQn);
}

/* ── IRQ handlers ────────────────────────────────────────────────────── */
void DMA1_Stream6_IRQHandler(void) {
    HAL_DMA_IRQHandler(&s_hdma_tx);
}

void I2C1_EV_IRQHandler(void) {
    HAL_I2C_EV_IRQHandler(&s_hi2c);
}

void HAL_I2C_MemTxCpltCallback(I2C_HandleTypeDef *hi2c) {
    if (hi2c->Instance != I2C1) return;
    s_busy = false;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_done, &woken);
    portYIELD_FROM_ISR(woken);
}

/* ── Public API ──────────────────────────────────────────────────────── */
void ssd1306_init(void) {
    i2c_hw_init();

    s_done = xSemaphoreCreateBinary();
    xSemaphoreGive(s_done);

    HAL_Delay(100);

    static const uint8_t init_seq[] = {
        0xAE,         /* display off */
        0xD5, 0x80,   /* clock divide */
        0xA8, 0x3F,   /* multiplex 64 */
        0xD3, 0x00,   /* offset 0 */
        0x40,         /* start line 0 */
        0x8D, 0x14,   /* charge pump on */
        0x20, 0x00,   /* horizontal addressing */
        0xA1,         /* seg remap */
        0xC8,         /* COM reversed */
        0xDA, 0x12,   /* COM pins */
        0x81, 0xCF,   /* contrast */
        0xD9, 0xF1,   /* pre-charge */
        0xDB, 0x40,   /* VCOMH */
        0xA4,         /* output from RAM */
        0xA6,         /* normal display */
        0xAF,         /* display on */
    };
    HAL_I2C_Mem_Write(&s_hi2c, OLED_I2C_ADDR << 1, 0x00,
                      I2C_MEMADD_SIZE_8BIT, (uint8_t *)init_seq,
                      sizeof(init_seq), 100);

    memset(buf_a, 0, SSD1306_BUF_SIZE);
    memset(buf_b, 0, SSD1306_BUF_SIZE);
    ssd1306_update();
}

void ssd1306_clear(void) {
    memset(back_buf, 0, SSD1306_BUF_SIZE);
}

bool ssd1306_busy(void) { return s_busy; }

void ssd1306_update(void) {
    xSemaphoreTake(s_done, pdMS_TO_TICKS(100));

    /* Swap buffers */
    uint8_t *tmp = front_buf;
    front_buf = back_buf;
    back_buf = tmp;

    /* Set address window */
    uint8_t addr_win[] = { 0x21, 0x00, 0x7F, 0x22, 0x00, 0x07 };
    HAL_I2C_Mem_Write(&s_hi2c, OLED_I2C_ADDR << 1, 0x00,
                      I2C_MEMADD_SIZE_8BIT, addr_win, sizeof(addr_win), 10);

    /* Build DMA buffer: 0x40 + pixel data */
    s_dma_buf[0] = 0x40;
    memcpy(&s_dma_buf[1], front_buf, SSD1306_BUF_SIZE);

    /* Start DMA transfer — returns immediately */
    s_busy = true;
    HAL_I2C_Mem_Write_DMA(&s_hi2c, OLED_I2C_ADDR << 1, 0x40,
                          I2C_MEMADD_SIZE_8BIT, &s_dma_buf[1], SSD1306_BUF_SIZE);
}

/* ── Draw primitives ─────────────────────────────────────────────────── */
void ssd1306_draw_pixel(int x, int y, bool on) {
    if ((unsigned)x >= SSD1306_WIDTH || (unsigned)y >= SSD1306_HEIGHT) return;
    int idx = x + (y / 8) * SSD1306_WIDTH;
    if (on) back_buf[idx] |=  (1u << (y % 8));
    else    back_buf[idx] &= ~(1u << (y % 8));
}

void ssd1306_draw_char(int x, int y, char ch, const FontDef *font) {
    if (ch < 32 || ch > 126) return;
    uint16_t ci = (uint16_t)(ch - 32) * (font->width - 1);
    for (uint8_t col = 0; col < font->width - 1; col++) {
        uint8_t col_data = font->data[ci + col];
        for (uint8_t row = 0; row < font->height; row++) {
            if ((col_data >> row) & 1)
                ssd1306_draw_pixel(x + col, y + row, true);
        }
    }
}

void ssd1306_draw_string(int x, int y, const char *str, const FontDef *font) {
    while (*str) {
        ssd1306_draw_char(x, y, *str++, font);
        x += font->width;
        if (x + font->width > SSD1306_WIDTH) break;
    }
}

void ssd1306_draw_hline(int x, int y, int w) {
    for (int i = 0; i < w; i++)
        ssd1306_draw_pixel(x + i, y, true);
}
