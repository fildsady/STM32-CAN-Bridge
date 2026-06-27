#ifndef __SSD1306_H__
#define __SSD1306_H__

#include <stdint.h>
#include <stdbool.h>
#include "font.h"

#define SSD1306_WIDTH       128
#define SSD1306_HEIGHT      64
#define SSD1306_BUFFER_SIZE (SSD1306_WIDTH * SSD1306_HEIGHT / 8)

#define SSD1306_COLOR_BLACK 0
#define SSD1306_COLOR_WHITE 1


uint8_t SSD1306_Init(void);
void    SSD1306_Clear(void);
void    SSD1306_UpdateScreen(void);
uint8_t SSD1306_IsBusy(void);

void SSD1306_DrawPixel   (int16_t x, int16_t y, uint8_t color);
void SSD1306_DrawChar    (int16_t x, int16_t y, char ch,        const FontDef *font, uint8_t color);
void SSD1306_DrawString  (int16_t x, int16_t y, const char *str, const FontDef *font, uint8_t color);
void SSD1306_DrawString2x(int16_t x, int16_t y, const char *str, const FontDef *font, uint8_t color);
void SSD1306_FillRect    (int16_t x, int16_t y, int16_t w, int16_t h, uint8_t color);
void SSD1306_InvertRect  (int16_t x, int16_t y, int16_t w, int16_t h);

#endif /* __SSD1306_H__ */
