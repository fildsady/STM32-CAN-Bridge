/*
 * STM32-CAN-Bridge — UART ↔ CAN transparent gateway
 *
 * Nucleo-F446RE: USART2 (VCP via ST-Link) ↔ CAN1 (PB8/PB9)
 * Protocol compatible with PicoCANBridge (same frame format)
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

    uart_init(115200);
    can_bridge_init(CAN_BAUD_125K);
    modbus_bridge_init(115200);

    const char *msg = "[Bridge] F446RE CAN+Modbus+UART ready\r\n";
    uart_write((const uint8_t *)msg, strlen(msg));

    for (;;) {
        can_bridge_poll();
        modbus_bridge_poll();
        LL_GPIO_TogglePin(GPIOA, LL_GPIO_PIN_5);
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

int main(void) {
    SystemClock_Config();
    led_init();

    xTaskCreate(task_bridge, "bridge", 512, NULL, 2, NULL);
    vTaskStartScheduler();

    while (1) {}
    return 0;
}

/* HAL needs this for CAN driver */
uint32_t HAL_GetTick(void) {
    return xTaskGetTickCount();
}
