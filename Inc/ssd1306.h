#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "font.h"

#define SSD1306_WIDTH   128
#define SSD1306_HEIGHT   64
#define SSD1306_BUF_SIZE (SSD1306_WIDTH * SSD1306_HEIGHT / 8)  /* 1024 */

#define OLED_I2C_ADDR   0x3C

void ssd1306_init(void);
void ssd1306_clear(void);
void ssd1306_update(void);
bool ssd1306_busy(void);

void ssd1306_draw_pixel(int x, int y, bool on);
void ssd1306_draw_char(int x, int y, char ch, const FontDef *font);
void ssd1306_draw_string(int x, int y, const char *str, const FontDef *font);
void ssd1306_draw_hline(int x, int y, int w);
