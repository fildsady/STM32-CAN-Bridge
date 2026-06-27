/*
 * uart_io.c — USART2 driver (ST-Link VCP on Nucleo-F446RE)
 * PA2=TX, PA3=RX, 115200 8N1
 */
#include "stm32f4xx.h"
#include "stm32f4xx_ll_bus.h"
#include "stm32f4xx_ll_gpio.h"
#include "stm32f4xx_ll_usart.h"
#include <string.h>

#define UART_BUF_SIZE  128

static volatile uint8_t  s_rx_buf[UART_BUF_SIZE];
static volatile uint16_t s_rx_head = 0;
static volatile uint16_t s_rx_tail = 0;

void uart_init(uint32_t baudrate) {
    LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_USART2);
    LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_GPIOA);

    /* PA2 = TX, PA3 = RX → AF7 (USART2) */
    LL_GPIO_SetPinMode(GPIOA, LL_GPIO_PIN_2, LL_GPIO_MODE_ALTERNATE);
    LL_GPIO_SetAFPin_0_7(GPIOA, LL_GPIO_PIN_2, LL_GPIO_AF_7);
    LL_GPIO_SetPinSpeed(GPIOA, LL_GPIO_PIN_2, LL_GPIO_SPEED_FREQ_HIGH);

    LL_GPIO_SetPinMode(GPIOA, LL_GPIO_PIN_3, LL_GPIO_MODE_ALTERNATE);
    LL_GPIO_SetAFPin_0_7(GPIOA, LL_GPIO_PIN_3, LL_GPIO_AF_7);
    LL_GPIO_SetPinPull(GPIOA, LL_GPIO_PIN_3, LL_GPIO_PULL_UP);

    LL_USART_SetBaudRate(USART2, SystemCoreClock / 4, LL_USART_OVERSAMPLING_16, baudrate);
    LL_USART_SetDataWidth(USART2, LL_USART_DATAWIDTH_8B);
    LL_USART_SetStopBitsLength(USART2, LL_USART_STOPBITS_1);
    LL_USART_SetParity(USART2, LL_USART_PARITY_NONE);
    LL_USART_SetTransferDirection(USART2, LL_USART_DIRECTION_TX_RX);

    LL_USART_EnableIT_RXNE(USART2);
    NVIC_SetPriority(USART2_IRQn, 6);
    NVIC_EnableIRQ(USART2_IRQn);

    LL_USART_Enable(USART2);
}

void USART2_IRQHandler(void) {
    if (LL_USART_IsActiveFlag_RXNE(USART2)) {
        uint8_t c = LL_USART_ReceiveData8(USART2);
        uint16_t next = (s_rx_head + 1) % UART_BUF_SIZE;
        if (next != s_rx_tail) {
            s_rx_buf[s_rx_head] = c;
            s_rx_head = next;
        }
    }
    if (LL_USART_IsActiveFlag_ORE(USART2)) {
        LL_USART_ClearFlag_ORE(USART2);
    }
}

uint16_t uart_available(void) {
    return (s_rx_head - s_rx_tail + UART_BUF_SIZE) % UART_BUF_SIZE;
}

uint8_t uart_read_byte(void) {
    if (s_rx_head == s_rx_tail) return 0;
    uint8_t c = s_rx_buf[s_rx_tail];
    s_rx_tail = (s_rx_tail + 1) % UART_BUF_SIZE;
    return c;
}

void uart_write(const uint8_t *data, uint16_t len) {
    for (uint16_t i = 0; i < len; i++) {
        while (!LL_USART_IsActiveFlag_TXE(USART2)) {}
        LL_USART_TransmitData8(USART2, data[i]);
    }
}

void uart_write_byte(uint8_t c) {
    while (!LL_USART_IsActiveFlag_TXE(USART2)) {}
    LL_USART_TransmitData8(USART2, c);
}
