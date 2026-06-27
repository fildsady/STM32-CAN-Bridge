#pragma once
#include <stdint.h>

void     uart_init(uint32_t baudrate);
uint16_t uart_available(void);
uint8_t  uart_read_byte(void);
void     uart_write(const uint8_t *data, uint16_t len);
void     uart_write_byte(uint8_t c);
