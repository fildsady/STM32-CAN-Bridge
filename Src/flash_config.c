/*
 * flash_config.c — EEPROM emulation on internal flash sector 3
 *
 * Sector 3: 0x0800C000, 16KB
 * Entry format: [key:1][len:1][data:N][padding to 4-byte align]
 * Scan from start, last entry with matching key = current value
 * Compact when sector full: copy latest values to RAM, erase, rewrite
 */
#include "flash_config.h"
#include "stm32f4xx.h"
#include <string.h>

#define FLASH_SECTOR_ADDR  0x0800C000u
#define FLASH_SECTOR_SIZE  0x4000u      /* 16KB */
#define FLASH_SECTOR_NUM   3
#define ENTRY_MAGIC        0xA5

typedef struct __attribute__((packed)) {
    uint8_t magic;
    uint8_t key;
    uint8_t len;
    uint8_t data[];   /* followed by len bytes, then pad to 4-byte */
} entry_t;

static uint32_t s_write_offset = 0;

/* ── Flash low-level ─────────────────────────────────────────────── */
static void flash_unlock(void) {
    if (FLASH->CR & FLASH_CR_LOCK) {
        FLASH->KEYR = 0x45670123u;
        FLASH->KEYR = 0xCDEF89ABu;
    }
}

static void flash_lock(void) { FLASH->CR |= FLASH_CR_LOCK; }

static void flash_wait(void) { while (FLASH->SR & FLASH_SR_BSY) {} }

static void flash_erase_sector3(void) {
    flash_wait();
    FLASH->CR &= ~FLASH_CR_PSIZE;
    FLASH->CR |= FLASH_CR_PSIZE_1;     /* 32-bit parallelism */
    FLASH->CR &= ~FLASH_CR_SNB;
    FLASH->CR |= (FLASH_SECTOR_NUM << FLASH_CR_SNB_Pos);
    FLASH->CR |= FLASH_CR_SER;
    FLASH->CR |= FLASH_CR_STRT;
    flash_wait();
    FLASH->CR &= ~FLASH_CR_SER;
}

static void flash_write_byte(uint32_t addr, uint8_t val) {
    flash_wait();
    FLASH->CR &= ~FLASH_CR_PSIZE;      /* 8-bit */
    FLASH->CR |= FLASH_CR_PG;
    *(__IO uint8_t *)addr = val;
    flash_wait();
    FLASH->CR &= ~FLASH_CR_PG;
}

static void flash_write_bytes(uint32_t addr, const uint8_t *data, uint16_t len) {
    for (uint16_t i = 0; i < len; i++)
        flash_write_byte(addr + i, data[i]);
}

/* ── Entry helpers ───────────────────────────────────────────────── */
static uint16_t entry_size(uint8_t data_len) {
    uint16_t s = 3 + data_len;         /* magic + key + len + data */
    return (s + 3) & ~3u;              /* pad to 4-byte */
}

static const entry_t *entry_at(uint32_t offset) {
    return (const entry_t *)(FLASH_SECTOR_ADDR + offset);
}

static bool entry_valid(uint32_t offset) {
    if (offset + 3 >= FLASH_SECTOR_SIZE) return false;
    const entry_t *e = entry_at(offset);
    if (e->magic != ENTRY_MAGIC) return false;
    if (offset + entry_size(e->len) > FLASH_SECTOR_SIZE) return false;
    return true;
}

/* ── Init: find write offset ─────────────────────────────────────── */
void eeprom_init(void) {
    /* Check if sector is blank (first boot or corrupted) */
    uint8_t first = *(volatile uint8_t *)FLASH_SECTOR_ADDR;
    if (first != 0xFF && first != ENTRY_MAGIC) {
        /* Sector not blank and no valid entry — erase it */
        __disable_irq();
        flash_unlock();
        flash_erase_sector3();
        flash_lock();
        __enable_irq();
    }

    s_write_offset = 0;
    while (entry_valid(s_write_offset)) {
        const entry_t *e = entry_at(s_write_offset);
        s_write_offset += entry_size(e->len);
    }
}

/* ── Write entry ─────────────────────────────────────────────────── */
static void write_entry(uint8_t key, const uint8_t *data, uint8_t len) {
    uint16_t needed = entry_size(len);

    /* Compact if not enough space */
    if (s_write_offset + needed > FLASH_SECTOR_SIZE - 4) {
        /* Read all latest values to RAM */
        uint8_t cfg_mb = eeprom_read_u8(CFG_KEY_MB_BAUD, 4);
        uint8_t cfg_can = eeprom_read_u8(CFG_KEY_CAN_BAUD, 2);
        uint32_t boot = eeprom_read_boot_count();

        /* Collect faults (keep last 50) */
        fault_entry_t faults[50];
        uint16_t fc = eeprom_read_faults(faults, 50);

        __disable_irq();
        flash_unlock();
        flash_erase_sector3();

        /* Rewrite config */
        s_write_offset = 0;
        uint8_t hdr[3];

        hdr[0] = ENTRY_MAGIC; hdr[1] = CFG_KEY_MB_BAUD; hdr[2] = 1;
        flash_write_bytes(FLASH_SECTOR_ADDR + s_write_offset, hdr, 3);
        flash_write_byte(FLASH_SECTOR_ADDR + s_write_offset + 3, cfg_mb);
        s_write_offset += entry_size(1);

        hdr[1] = CFG_KEY_CAN_BAUD;
        flash_write_bytes(FLASH_SECTOR_ADDR + s_write_offset, hdr, 3);
        flash_write_byte(FLASH_SECTOR_ADDR + s_write_offset + 3, cfg_can);
        s_write_offset += entry_size(1);

        hdr[1] = CFG_KEY_BOOT_COUNT; hdr[2] = 4;
        flash_write_bytes(FLASH_SECTOR_ADDR + s_write_offset, hdr, 3);
        boot++;
        flash_write_bytes(FLASH_SECTOR_ADDR + s_write_offset + 3, (uint8_t *)&boot, 4);
        s_write_offset += entry_size(4);

        /* Rewrite faults */
        for (uint16_t i = 0; i < fc; i++) {
            hdr[1] = CFG_KEY_FAULT; hdr[2] = sizeof(fault_entry_t);
            flash_write_bytes(FLASH_SECTOR_ADDR + s_write_offset, hdr, 3);
            flash_write_bytes(FLASH_SECTOR_ADDR + s_write_offset + 3,
                              (uint8_t *)&faults[i], sizeof(fault_entry_t));
            s_write_offset += entry_size(sizeof(fault_entry_t));
        }

        flash_lock();
        __enable_irq();
    }

    /* Write new entry — disable interrupts during flash write */
    __disable_irq();
    flash_unlock();
    uint8_t hdr[3] = { ENTRY_MAGIC, key, len };
    flash_write_bytes(FLASH_SECTOR_ADDR + s_write_offset, hdr, 3);
    if (len > 0)
        flash_write_bytes(FLASH_SECTOR_ADDR + s_write_offset + 3, data, len);
    flash_lock();
    __enable_irq();

    s_write_offset += needed;
}

/* ── Public API ──────────────────────────────────────────────────── */
uint8_t eeprom_read_u8(uint8_t key, uint8_t def) {
    uint8_t val = def;
    uint32_t off = 0;
    while (entry_valid(off)) {
        const entry_t *e = entry_at(off);
        if (e->key == key && e->len >= 1) val = e->data[0];
        off += entry_size(e->len);
    }
    return val;
}

void eeprom_write_u8(uint8_t key, uint8_t val) {
    write_entry(key, &val, 1);
}

void eeprom_write_fault(const fault_entry_t *fault) {
    write_entry(CFG_KEY_FAULT, (const uint8_t *)fault, sizeof(fault_entry_t));
}

uint16_t eeprom_read_faults(fault_entry_t *out, uint16_t max_count) {
    uint16_t count = 0;
    uint32_t off = 0;
    while (entry_valid(off)) {
        const entry_t *e = entry_at(off);
        if (e->key == CFG_KEY_FAULT && e->len == sizeof(fault_entry_t)) {
            if (count < max_count)
                memcpy(&out[count], e->data, sizeof(fault_entry_t));
            count++;
        }
        off += entry_size(e->len);
    }
    /* Return last max_count entries */
    if (count > max_count) {
        uint16_t skip = count - max_count;
        count = 0;
        off = 0;
        uint16_t idx = 0;
        while (entry_valid(off)) {
            const entry_t *e = entry_at(off);
            if (e->key == CFG_KEY_FAULT && e->len == sizeof(fault_entry_t)) {
                if (idx >= skip)
                    memcpy(&out[count++], e->data, sizeof(fault_entry_t));
                idx++;
            }
            off += entry_size(e->len);
        }
    }
    return count;
}

uint32_t eeprom_read_boot_count(void) {
    uint32_t val = 0;
    uint32_t off = 0;
    while (entry_valid(off)) {
        const entry_t *e = entry_at(off);
        if (e->key == CFG_KEY_BOOT_COUNT && e->len >= 4)
            memcpy(&val, e->data, 4);
        off += entry_size(e->len);
    }
    return val;
}
