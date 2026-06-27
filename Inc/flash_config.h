#pragma once
#include <stdint.h>
#include <stdbool.h>

/* EEPROM emulation on STM32F446RE internal flash
 * Sector 3 (0x0800C000, 16KB) — survives code reflash
 *
 * Keys:
 *   0x01 = Modbus baud index (0-6)
 *   0x02 = CAN baud index (0-5)
 *   0x03 = Boot count
 *   0x10 = Fault entry (type, PC, LR, uptime)
 */

#define CFG_KEY_MB_BAUD    0x01
#define CFG_KEY_CAN_BAUD   0x02
#define CFG_KEY_BOOT_COUNT 0x03
#define CFG_KEY_FAULT      0x10

/* Fault types */
#define FAULT_HARDFAULT    0x01
#define FAULT_STACKOVERFLOW 0x02
#define FAULT_WDT_RESET    0x03
#define FAULT_ASSERT       0x04

typedef struct __attribute__((packed)) {
    uint8_t  type;
    uint32_t pc;
    uint32_t lr;
    uint32_t uptime;
    uint32_t cfsr;
} fault_entry_t;

void eeprom_init(void);
uint8_t eeprom_read_u8(uint8_t key, uint8_t def);
void eeprom_write_u8(uint8_t key, uint8_t val);
void eeprom_write_fault(const fault_entry_t *fault);
uint16_t eeprom_read_faults(fault_entry_t *out, uint16_t max_count);
uint32_t eeprom_read_boot_count(void);
