/*
 * fault_handler.c — Hard Fault, Stack Overflow, WDT handlers
 * Save fault info to EEPROM emulation before reset
 */
#include "flash_config.h"
#include "stm32f4xx.h"
#include "FreeRTOS.h"
#include "task.h"

void HardFault_Handler(void) {
    __asm volatile (
        "tst lr, #4        \n"
        "ite eq             \n"
        "mrseq r0, msp      \n"
        "mrsne r0, psp      \n"
        "b hard_fault_save  \n"
    );
}

void __attribute__((used)) hard_fault_save(uint32_t *stack) {
    fault_entry_t f;
    f.type = FAULT_HARDFAULT;
    f.pc   = stack[6];
    f.lr   = stack[5];
    f.uptime = xTaskGetTickCountFromISR() / configTICK_RATE_HZ;
    f.cfsr = SCB->CFSR;
    eeprom_write_fault(&f);
    NVIC_SystemReset();
}

void vApplicationStackOverflowHook(TaskHandle_t task, char *name) {
    (void)task; (void)name;
    fault_entry_t f;
    f.type = FAULT_STACKOVERFLOW;
    f.pc   = 0;
    f.lr   = (uint32_t)__builtin_return_address(0);
    f.uptime = xTaskGetTickCountFromISR() / configTICK_RATE_HZ;
    f.cfsr = 0;
    eeprom_write_fault(&f);
    NVIC_SystemReset();
}
