/*
 * uart_io.c — USART2 driver (ST-Link VCP on Nucleo-F446RE)
 * PA2=TX, PA3=RX, 115200 8N1
 * RX: DMA + IDLE (handled by modbus_bridge), TX: blocking
 */
#include "stm32f4xx.h"
#include "stm32f4xx_ll_bus.h"
#include "stm32f4xx_ll_gpio.h"
#include "stm32f4xx_ll_usart.h"
#include <string.h>

extern void modbus_bridge_usart2_idle(void);

void uart_init(uint32_t baudrate) {
    LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_USART2);
    LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_GPIOA);

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

    NVIC_SetPriority(USART2_IRQn, 5);
    NVIC_EnableIRQ(USART2_IRQn);

    LL_USART_Enable(USART2);
}

void USART2_IRQHandler(void) {
    if (LL_USART_IsActiveFlag_IDLE(USART2)) {
        LL_USART_ClearFlag_IDLE(USART2);
        modbus_bridge_usart2_idle();
    }
    if (LL_USART_IsActiveFlag_ORE(USART2)) {
        LL_USART_ClearFlag_ORE(USART2);
    }
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
