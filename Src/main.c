/*
 * STM32-CAN-Bridge — UART ↔ CAN ↔ Modbus RS-485 gateway
 *
 * Nucleo-F446RE: USART2 (VCP) ↔ CAN1 (PA11/PA12) ↔ USART1 RS-485 (PA9/PA10)
 * OLED SSD1306 I2C1 (PB8/PB9 = D15/D14)
 * All LL drivers — no HAL
 */
#include "stm32f4xx.h"
#include "stm32f4xx_ll_rcc.h"
#include "stm32f4xx_ll_bus.h"
#include "stm32f4xx_ll_system.h"
#include "stm32f4xx_ll_cortex.h"
#include "stm32f4xx_ll_utils.h"
#include "stm32f4xx_ll_pwr.h"
#include "stm32f4xx_ll_gpio.h"

#include "FreeRTOS.h"
#include "task.h"

#include "can_bridge.h"
#include "modbus_bridge.h"
#include "uart_io.h"
#include "ssd1306.h"

#include <stdio.h>
#include <string.h>

void SystemClock_Config(void) {
    LL_RCC_HSE_Enable();
    while (LL_RCC_HSE_IsReady() != 1) {}

    LL_FLASH_SetLatency(LL_FLASH_LATENCY_5);
    while (LL_FLASH_GetLatency() != LL_FLASH_LATENCY_5) {}

    LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_PWR);
    LL_PWR_SetRegulVoltageScaling(LL_PWR_REGU_VOLTAGE_SCALE1);

    /* HSE(8MHz) / 8 * 360 / 2 = 180MHz */
    LL_RCC_PLL_ConfigDomain_SYS(LL_RCC_PLLSOURCE_HSE, LL_RCC_PLLM_DIV_8, 360, LL_RCC_PLLP_DIV_2);
    LL_RCC_PLL_Enable();
    while (LL_RCC_PLL_IsReady() != 1) {}

    LL_PWR_EnableOverDriveMode();
    while (LL_PWR_IsActiveFlag_OD() != 1) {}
    LL_PWR_EnableOverDriveSwitching();
    while (LL_PWR_IsActiveFlag_ODSW() != 1) {}

    LL_RCC_SetAHBPrescaler(LL_RCC_SYSCLK_DIV_1);
    LL_RCC_SetAPB1Prescaler(LL_RCC_APB1_DIV_4);   /* APB1 = 45MHz */
    LL_RCC_SetAPB2Prescaler(LL_RCC_APB2_DIV_2);

    LL_RCC_SetSysClkSource(LL_RCC_SYS_CLKSOURCE_PLL);
    while (LL_RCC_GetSysClkSource() != LL_RCC_SYS_CLKSOURCE_STATUS_PLL) {}

    LL_SetSystemCoreClock(180000000);
}

static void led_init(void) {
    LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_GPIOA);
    LL_GPIO_SetPinMode(GPIOA, LL_GPIO_PIN_5, LL_GPIO_MODE_OUTPUT);
    LL_GPIO_SetPinSpeed(GPIOA, LL_GPIO_PIN_5, LL_GPIO_SPEED_FREQ_LOW);
}

static void task_bridge(void *pv) {
    (void)pv;

    uart_init(460800);
    can_bridge_init(CAN_BAUD_125K);
    modbus_bridge_init(115200);

    const char *msg = "[Bridge] F446RE CAN+Modbus+UART ready\r\n";
    uart_write((const uint8_t *)msg, strlen(msg));

    for (;;) {
        modbus_bridge_relay();
        vTaskDelay(1);
    }
}

static void task_oled(void *pv) {
    (void)pv;
    char buf[22];

    SSD1306_Clear();
    SSD1306_DrawString(0, 0, "STM32-CAN-Bridge", &Font_6x8, 1);
    SSD1306_DrawString(0, 12, "Modbus Relay", &Font_6x8, 1);
    SSD1306_DrawString(0, 24, "Ready.", &Font_6x8, 1);
    SSD1306_UpdateScreen();

    for (;;) {
        SSD1306_Clear();
        SSD1306_DrawString(0, 0, "STM32-CAN-Bridge", &Font_6x8, 1);

        snprintf(buf, sizeof(buf), "PC>485: %lu", (unsigned long)modbus_bridge_pc_count());
        SSD1306_DrawString(0, 16, buf, &Font_6x8, 1);

        snprintf(buf, sizeof(buf), "485>PC: %lu", (unsigned long)modbus_bridge_rs485_count());
        SSD1306_DrawString(0, 28, buf, &Font_6x8, 1);

        static const char* bauds[] = {"9.6k","19.2k","38.4k","57.6k","115k","230k","460k"};
        uint8_t bi = modbus_bridge_get_baud_idx();
        snprintf(buf, sizeof(buf), "PC:460k RS485:%s", bi < 7 ? bauds[bi] : "?");
        SSD1306_DrawString(0, 40, buf, &Font_6x8, 1);

        LL_GPIO_TogglePin(GPIOA, LL_GPIO_PIN_5);
        SSD1306_UpdateScreen();
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

int main(void) {
    SystemClock_Config();
    LL_Init1msTick(180000000);
    led_init();
    SSD1306_Init();

    xTaskCreate(task_bridge, "bridge", 2048, NULL, 3, NULL);  /* high priority — relay must not be blocked */
    xTaskCreate(task_oled,  "oled",   1024, NULL, 1, NULL);  /* low priority */
    vTaskStartScheduler();

    while (1) {}
    return 0;
}
