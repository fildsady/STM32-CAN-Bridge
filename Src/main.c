/*
 * STM32-CAN-Bridge — Dual mode: Relay / Gateway
 *
 * Nucleo-F446RE: USART2 (VCP) ↔ CAN1 (PA11/PA12) ↔ USART1 RS-485 (PA9/PA10)
 * OLED SSD1306 I2C1 (PB8/PB9 = D15/D14)
 * User button PC13 = toggle mode
 * All LL drivers — no HAL
 *
 * RELAY mode:   transparent UART↔RS-485 byte forwarding (DMA+IDLE)
 * GATEWAY mode: protocol-based 0x01=CAN TX, 0x02=CAN RX, 0x03=Modbus TX, 0x04=Modbus RX
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
#include "flash_config.h"
#include "uart_io.h"
#include "ssd1306.h"

#include <stdio.h>
#include <string.h>

volatile bool g_gateway_mode = true;
volatile bool g_boot_debug = true;

void SystemClock_Config(void) {
    LL_RCC_HSE_Enable();
    while (LL_RCC_HSE_IsReady() != 1) {}

    LL_FLASH_SetLatency(LL_FLASH_LATENCY_5);
    while (LL_FLASH_GetLatency() != LL_FLASH_LATENCY_5) {}

    LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_PWR);
    LL_PWR_SetRegulVoltageScaling(LL_PWR_REGU_VOLTAGE_SCALE1);

    LL_RCC_PLL_ConfigDomain_SYS(LL_RCC_PLLSOURCE_HSE, LL_RCC_PLLM_DIV_8, 360, LL_RCC_PLLP_DIV_2);
    LL_RCC_PLL_Enable();
    while (LL_RCC_PLL_IsReady() != 1) {}

    LL_PWR_EnableOverDriveMode();
    while (LL_PWR_IsActiveFlag_OD() != 1) {}
    LL_PWR_EnableOverDriveSwitching();
    while (LL_PWR_IsActiveFlag_ODSW() != 1) {}

    LL_RCC_SetAHBPrescaler(LL_RCC_SYSCLK_DIV_1);
    LL_RCC_SetAPB1Prescaler(LL_RCC_APB1_DIV_4);
    LL_RCC_SetAPB2Prescaler(LL_RCC_APB2_DIV_2);

    LL_RCC_SetSysClkSource(LL_RCC_SYS_CLKSOURCE_PLL);
    while (LL_RCC_GetSysClkSource() != LL_RCC_SYS_CLKSOURCE_STATUS_PLL) {}

    LL_SetSystemCoreClock(180000000);
}

static void hw_init(void) {
    /* LED PA5 */
    LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_GPIOA);
    LL_GPIO_SetPinMode(GPIOA, LL_GPIO_PIN_5, LL_GPIO_MODE_OUTPUT);
    LL_GPIO_SetPinSpeed(GPIOA, LL_GPIO_PIN_5, LL_GPIO_SPEED_FREQ_LOW);

    /* User button PC13 (active low on Nucleo) */
    LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_GPIOC);
    LL_GPIO_SetPinMode(GPIOC, LL_GPIO_PIN_13, LL_GPIO_MODE_INPUT);
    LL_GPIO_SetPinPull(GPIOC, LL_GPIO_PIN_13, LL_GPIO_PULL_NO);
}

static void task_bridge(void *pv) {
    (void)pv;

    eeprom_init();
    uint8_t mb_idx = eeprom_read_u8(CFG_KEY_MB_BAUD, 4);
    uint8_t can_idx = eeprom_read_u8(CFG_KEY_CAN_BAUD, 2);
    uart_init(921600);
    can_bridge_init(can_idx);
    modbus_bridge_init(115200);
    modbus_bridge_set_baud(mb_idx);

    /* Show saved config on OLED */
    char dbg[32];
    SSD1306_Clear();
    snprintf(dbg, sizeof(dbg), "MB:%d CAN:%d", mb_idx, can_idx);
    SSD1306_DrawString(0, 0, dbg, &Font_6x8, 1);
    SSD1306_DrawString(0, 12, "Config loaded", &Font_6x8, 1);
    SSD1306_UpdateScreen();
    vTaskDelay(pdMS_TO_TICKS(2000));
    g_boot_debug = false;

    for (;;) {
        if (g_gateway_mode) {
            can_bridge_poll();
            modbus_bridge_poll();
        } else {
            modbus_bridge_relay();
        }
        vTaskDelay(1);
    }
}

static void task_oled(void *pv) {
    (void)pv;
    char buf[22];
    bool last_btn = true;

    while (g_boot_debug) vTaskDelay(pdMS_TO_TICKS(100));

    for (;;) {
        /* Button debounce — PC13 active low */
        bool btn = LL_GPIO_IsInputPinSet(GPIOC, LL_GPIO_PIN_13) == 0;
        if (btn && !last_btn) {
            g_gateway_mode = !g_gateway_mode;
            modbus_bridge_set_mode(g_gateway_mode);
        }
        last_btn = btn;

        /* Draw */
        SSD1306_Clear();
        SSD1306_DrawString2x(0, 0, g_gateway_mode ? "GATEWAY" : "RELAY", &Font_6x8, 1);

        if (g_gateway_mode) {
            snprintf(buf, sizeof(buf), "CAN TX:%lu RX:%lu",
                     (unsigned long)can_bridge_tx_count(),
                     (unsigned long)can_bridge_rx_count());
            SSD1306_DrawString(0, 20, buf, &Font_6x8, 1);

            snprintf(buf, sizeof(buf), "MB  TX:%lu RX:%lu",
                     (unsigned long)modbus_bridge_pc_count(),
                     (unsigned long)modbus_bridge_rs485_count());
            SSD1306_DrawString(0, 32, buf, &Font_6x8, 1);

        } else {
            snprintf(buf, sizeof(buf), "PC>485: %lu", (unsigned long)modbus_bridge_pc_count());
            SSD1306_DrawString(0, 20, buf, &Font_6x8, 1);

            snprintf(buf, sizeof(buf), "485>PC: %lu", (unsigned long)modbus_bridge_rs485_count());
            SSD1306_DrawString(0, 32, buf, &Font_6x8, 1);
        }

        static const char* mbaud[] = {"9.6k","19.2k","38.4k","57.6k","115k","230k","460k"};
        static const char* cbaud[] = {"20k","50k","125k","250k","500k","1M"};
        uint8_t mbi = modbus_bridge_get_baud_idx();
        uint8_t cbi = can_bridge_get_baud_idx();
        snprintf(buf, sizeof(buf), "MB:%s CAN:%s", mbi < 7 ? mbaud[mbi] : "?", cbi < 6 ? cbaud[cbi] : "?");
        SSD1306_DrawString(0, 44, buf, &Font_6x8, 1);
        SSD1306_DrawString(0, 56, "PC:921k  BTN=mode", &Font_6x8, 1);

        LL_GPIO_TogglePin(GPIOA, LL_GPIO_PIN_5);
        SSD1306_UpdateScreen();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

int main(void) {
    SystemClock_Config();
    LL_Init1msTick(180000000);
    hw_init();
    SSD1306_Init();

    xTaskCreate(task_bridge, "bridge", 2048, NULL, 3, NULL);
    xTaskCreate(task_oled,  "oled",   1024, NULL, 1, NULL);
    vTaskStartScheduler();

    while (1) {}
    return 0;
}
