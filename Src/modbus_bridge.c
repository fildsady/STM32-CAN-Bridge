/*
 * modbus_bridge.c — Modbus RTU Master via USART1 + RS-485
 *
 * PA9=TX (D8), PA10=RX (D2), PA8=DE (D7)
 * Supports: raw pass-through + CAN↔Modbus translation
 */
#include "modbus_bridge.h"
#include "uart_io.h"
#include "can_bridge.h"
#include "stm32f4xx.h"
#include "stm32f4xx_ll_bus.h"
#include "stm32f4xx_ll_gpio.h"
#include "stm32f4xx_ll_usart.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>

/* ── Modbus CRC16 ────────────────────────────────────────────────────── */
static uint16_t crc16(const uint8_t *data, uint16_t len) {
    uint16_t crc = 0xFFFF;
    for (uint16_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++)
            crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
    }
    return crc;
}

/* ── USART1 RX buffer (IRQ) ──────────────────────────────────────────── */
#define MB_BUF_SIZE 128
static volatile uint8_t  s_rx_buf[MB_BUF_SIZE];
static volatile uint16_t s_rx_head = 0;
static volatile uint16_t s_rx_tail = 0;
static volatile uint32_t s_last_rx_us = 0;

/* Response buffer for poll_slave */
static uint8_t  s_resp_buf[MB_BUF_SIZE];
static uint16_t s_resp_len = 0;
static volatile bool s_resp_ready = false;

/* ── USART1 IRQ ──────────────────────────────────────────────────────── */
void USART1_IRQHandler(void) {
    if (LL_USART_IsActiveFlag_RXNE(USART1)) {
        uint8_t c = LL_USART_ReceiveData8(USART1);
        uint16_t next = (s_rx_head + 1) % MB_BUF_SIZE;
        if (next != s_rx_tail) {
            s_rx_buf[s_rx_head] = c;
            s_rx_head = next;
        }
        s_last_rx_us = DWT->CYCCNT;
    }
    if (LL_USART_IsActiveFlag_ORE(USART1)) {
        LL_USART_ClearFlag_ORE(USART1);
    }
}

/* ── Init ─────────────────────────────────────────────────────────────── */
void modbus_bridge_init(uint32_t baudrate) {
    LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_USART1);
    LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_GPIOA);

    /* PA9 = TX (D8), PA10 = RX (D2) → AF7 (USART1) */
    LL_GPIO_SetPinMode(GPIOA, LL_GPIO_PIN_9, LL_GPIO_MODE_ALTERNATE);
    LL_GPIO_SetAFPin_8_15(GPIOA, LL_GPIO_PIN_9, LL_GPIO_AF_7);
    LL_GPIO_SetPinSpeed(GPIOA, LL_GPIO_PIN_9, LL_GPIO_SPEED_FREQ_HIGH);

    LL_GPIO_SetPinMode(GPIOA, LL_GPIO_PIN_10, LL_GPIO_MODE_ALTERNATE);
    LL_GPIO_SetAFPin_8_15(GPIOA, LL_GPIO_PIN_10, LL_GPIO_AF_7);
    LL_GPIO_SetPinPull(GPIOA, LL_GPIO_PIN_10, LL_GPIO_PULL_UP);

    /* PA8 = DE (D7) — RS-485 direction */
    LL_GPIO_SetPinMode(GPIOA, LL_GPIO_PIN_8, LL_GPIO_MODE_OUTPUT);
    LL_GPIO_SetPinSpeed(GPIOA, LL_GPIO_PIN_8, LL_GPIO_SPEED_FREQ_HIGH);
    LL_GPIO_ResetOutputPin(GPIOA, LL_GPIO_PIN_8);  /* RX mode */

    /* USART1 on APB2 (90MHz) */
    LL_USART_SetBaudRate(USART1, SystemCoreClock / 2, LL_USART_OVERSAMPLING_16, baudrate);
    LL_USART_SetDataWidth(USART1, LL_USART_DATAWIDTH_8B);
    LL_USART_SetStopBitsLength(USART1, LL_USART_STOPBITS_1);
    LL_USART_SetParity(USART1, LL_USART_PARITY_NONE);
    LL_USART_SetTransferDirection(USART1, LL_USART_DIRECTION_TX_RX);

    LL_USART_EnableIT_RXNE(USART1);
    NVIC_SetPriority(USART1_IRQn, 6);
    NVIC_EnableIRQ(USART1_IRQn);

    LL_USART_Enable(USART1);

    /* Enable DWT cycle counter for 3.5-char timing */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

/* ── DE control ──────────────────────────────────────────────────────── */
static void de_tx(void) { LL_GPIO_SetOutputPin(MODBUS_DE_PORT, MODBUS_DE_PIN); }
static void de_rx(void) {
    /* Wait for UART TX complete before switching to RX */
    while (!LL_USART_IsActiveFlag_TC(USART1)) {}
    LL_GPIO_ResetOutputPin(MODBUS_DE_PORT, MODBUS_DE_PIN);
}

/* ── Raw TX (with DE toggle) ─────────────────────────────────────────── */
static void modbus_tx(const uint8_t *data, uint16_t len) {
    de_tx();
    for (uint16_t i = 0; i < len; i++) {
        while (!LL_USART_IsActiveFlag_TXE(USART1)) {}
        LL_USART_TransmitData8(USART1, data[i]);
    }
    de_rx();
}

void modbus_bridge_send_raw(const uint8_t *data, uint16_t len) {
    modbus_tx(data, len);
}

/* ── Collect response with 3.5-char timeout ──────────────────────────── */
static bool wait_response(uint16_t timeout_ms) {
    uint32_t start = xTaskGetTickCount();
    s_resp_len = 0;
    s_resp_ready = false;

    /* Wait for first byte */
    while (s_rx_head == s_rx_tail) {
        if ((xTaskGetTickCount() - start) > pdMS_TO_TICKS(timeout_ms)) return false;
        vTaskDelay(1);
    }

    /* Collect bytes until 3.5-char silence (~2ms @ 115200) */
    uint32_t t35_cycles = SystemCoreClock / 115200 * 11 * 4;
    while (s_resp_len < MB_BUF_SIZE) {
        if (s_rx_head != s_rx_tail) {
            s_resp_buf[s_resp_len++] = s_rx_buf[s_rx_tail];
            s_rx_tail = (s_rx_tail + 1) % MB_BUF_SIZE;
        } else {
            uint32_t elapsed = DWT->CYCCNT - s_last_rx_us;
            if (elapsed > t35_cycles && s_resp_len > 0) break;
            if ((xTaskGetTickCount() - start) > pdMS_TO_TICKS(timeout_ms)) break;
        }
    }

    /* Verify CRC */
    if (s_resp_len >= 4) {
        uint16_t rx_crc = (uint16_t)s_resp_buf[s_resp_len - 2] |
                          ((uint16_t)s_resp_buf[s_resp_len - 1] << 8);
        if (crc16(s_resp_buf, s_resp_len - 2) == rx_crc) {
            s_resp_ready = true;
            return true;
        }
    }
    return false;
}

/* ── FC03: Read Holding Registers ────────────────────────────────────── */
static bool read_holding(uint8_t slave, uint16_t start, uint16_t count,
                         uint16_t *out, uint16_t *out_count) {
    uint8_t req[8];
    req[0] = slave;
    req[1] = 0x03;
    req[2] = (uint8_t)(start >> 8);
    req[3] = (uint8_t)(start & 0xFF);
    req[4] = (uint8_t)(count >> 8);
    req[5] = (uint8_t)(count & 0xFF);
    uint16_t crc = crc16(req, 6);
    req[6] = crc & 0xFF;
    req[7] = crc >> 8;

    s_rx_head = s_rx_tail = 0;
    modbus_tx(req, 8);

    if (!wait_response(200)) return false;
    if (s_resp_buf[1] != 0x03) return false;

    uint8_t bytes = s_resp_buf[2];
    uint16_t n = bytes / 2;
    if (n > count) n = count;
    for (uint16_t i = 0; i < n; i++)
        out[i] = ((uint16_t)s_resp_buf[3 + i * 2] << 8) | s_resp_buf[3 + i * 2 + 1];
    *out_count = n;
    return true;
}

/* ── FC06: Write Single Register ─────────────────────────────────────── */
static bool write_single(uint8_t slave, uint16_t addr, uint16_t value) {
    uint8_t req[8];
    req[0] = slave;
    req[1] = 0x06;
    req[2] = (uint8_t)(addr >> 8);
    req[3] = (uint8_t)(addr & 0xFF);
    req[4] = (uint8_t)(value >> 8);
    req[5] = (uint8_t)(value & 0xFF);
    uint16_t crc = crc16(req, 6);
    req[6] = crc & 0xFF;
    req[7] = crc >> 8;

    s_rx_head = s_rx_tail = 0;
    modbus_tx(req, 8);
    wait_response(200);
    return true;
}

/* ── CAN → Modbus translation ────────────────────────────────────────── */
/* CAN command data[0] maps to Modbus register write */
bool modbus_bridge_can_to_modbus(uint32_t can_id, const uint8_t *data, uint8_t dlc) {
    if (dlc < 2) return false;

    /* CAN ID 0x100+N → Modbus slave N */
    uint8_t slave = (uint8_t)(can_id & 0x7F);
    if (slave == 0) slave = 1;
    uint8_t cmd = data[0];
    uint8_t val = data[1];

    switch (cmd) {
    case 0x01: return write_single(slave, 0x0010, 1);      /* Play */
    case 0x02: return write_single(slave, 0x0010, 2);      /* Stop */
    case 0x03: return write_single(slave, 0x0010, 3);      /* Next */
    case 0x04: return write_single(slave, 0x0010, 4);      /* Prev */
    case 0x05: return write_single(slave, 0x0010, 5);      /* Pause */
    case 0x06: return write_single(slave, 0x0003, val);    /* Volume */
    case 0x07: return write_single(slave, 0x0004, val);    /* Repeat */
    case 0x08: return write_single(slave, 0x0005, val);    /* Mono */
    case 0x09: return write_single(slave, 0x0006, val);    /* Autoplay */
    case 0x10: /* GET → read status */
        return modbus_bridge_poll_slave(slave, NULL, NULL);
    default:
        return false;
    }
}

/* ── Poll Modbus slave → format as CAN status ─────────────────────────── */
bool modbus_bridge_poll_slave(uint8_t slave_id, uint8_t *out_data, uint8_t *out_len) {
    uint16_t regs[9];
    uint16_t count = 0;

    if (!read_holding(slave_id, 0x0000, 9, regs, &count)) return false;
    if (count < 7) return false;

    /* Forward as CAN status frame 0x300+slave */
    uint8_t can_data[8];
    can_data[0] = (uint8_t)regs[0];   /* state */
    can_data[1] = (uint8_t)regs[1];   /* track */
    can_data[2] = (uint8_t)regs[3];   /* volume */
    can_data[3] = (uint8_t)regs[4];   /* repeat */
    can_data[4] = 0;                   /* sample_khz (not in basic regs) */
    can_data[5] = 0;                   /* temp */
    can_data[6] = 0;                   /* group */
    can_data[7] = 0;

    can_bridge_send(0x300 + slave_id, can_data, 8);

    if (out_data) memcpy(out_data, can_data, 8);
    if (out_len) *out_len = 8;
    return true;
}

/* ── Poll: handle PC pass-through + CAN→Modbus forwarding ────────────── */
void modbus_bridge_poll(void) {
    /* Forward Modbus response to PC via UART (as 0x04 frame) */
    if (s_resp_ready) {
        uint8_t pkt[2 + MB_BUF_SIZE];
        pkt[0] = BRIDGE_CMD_MODBUS_RX;
        pkt[1] = (uint8_t)s_resp_len;
        memcpy(&pkt[2], s_resp_buf, s_resp_len);
        uart_write(pkt, 2 + s_resp_len);
        s_resp_ready = false;
    }
}
