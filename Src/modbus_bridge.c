/*
 * modbus_bridge.c — Transparent Modbus relay via DMA + IDLE line
 *
 * USART1 (RS-485): PA9=TX, PA10=RX, PA8=DE — DMA2 Stream2 Ch4 RX
 * USART2 (VCP):    PA2=TX, PA3=RX          — DMA1 Stream5 Ch4 RX
 *
 * Flow: UART idle line IRQ fires when frame complete → forward buffer
 * No polling, no silence timer, hardware handles framing
 */
#include "modbus_bridge.h"
#include "flash_config.h"
#include "can_bridge.h"
#include "uart_io.h"
#include "stm32f4xx.h"
#include "stm32f4xx_ll_bus.h"
#include "stm32f4xx_ll_gpio.h"
#include "stm32f4xx_ll_usart.h"
#include "stm32f4xx_ll_dma.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include <string.h>

#define BUF_SIZE 256

/* ── PC → RS-485 (USART2 RX DMA → USART1 TX) ────────────────────────── */
uint8_t s_pc_dma_buf[BUF_SIZE];
volatile uint16_t s_pc_frame_len = 0;
volatile bool s_pc_frame_ready = false;

/* ── RS-485 → PC (USART1 RX DMA → USART2 TX) ────────────────────────── */
static uint8_t s_rs_dma_buf[BUF_SIZE];
static volatile uint16_t s_rs_frame_len = 0;
static volatile bool s_rs_frame_ready = false;

static volatile uint32_t s_pc_frames = 0;
static volatile uint32_t s_rs485_frames = 0;

uint32_t modbus_bridge_pc_count(void) { return s_pc_frames; }
uint32_t modbus_bridge_rs485_count(void) { return s_rs485_frames; }

/* ── DE control ──────────────────────────────────────────────────────── */
static void de_tx(void) { LL_GPIO_SetOutputPin(GPIOA, LL_GPIO_PIN_8); }
static void de_rx(void) {
    while (!LL_USART_IsActiveFlag_TC(USART1)) {}
    LL_GPIO_ResetOutputPin(GPIOA, LL_GPIO_PIN_8);
}

/* ── USART1 TX blocking (RS-485 out) ─────────────────────────────────── */
static void rs485_tx(const uint8_t *data, uint16_t len) {
    de_tx();
    for (uint16_t i = 0; i < len; i++) {
        while (!LL_USART_IsActiveFlag_TXE(USART1)) {}
        LL_USART_TransmitData8(USART1, data[i]);
    }
    de_rx();
}

/* ── Init ─────────────────────────────────────────────────────────────── */
void modbus_bridge_init(uint32_t baudrate) {
    /* ── USART1: RS-485 ──────────────────────────────────────────────── */
    LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_USART1);
    LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_GPIOA);
    LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_DMA2);

    /* PA9=TX, PA10=RX → AF7 */
    LL_GPIO_SetPinMode(GPIOA, LL_GPIO_PIN_9, LL_GPIO_MODE_ALTERNATE);
    LL_GPIO_SetAFPin_8_15(GPIOA, LL_GPIO_PIN_9, LL_GPIO_AF_7);
    LL_GPIO_SetPinSpeed(GPIOA, LL_GPIO_PIN_9, LL_GPIO_SPEED_FREQ_HIGH);
    LL_GPIO_SetPinMode(GPIOA, LL_GPIO_PIN_10, LL_GPIO_MODE_ALTERNATE);
    LL_GPIO_SetAFPin_8_15(GPIOA, LL_GPIO_PIN_10, LL_GPIO_AF_7);
    LL_GPIO_SetPinPull(GPIOA, LL_GPIO_PIN_10, LL_GPIO_PULL_UP);

    /* PA8=DE */
    LL_GPIO_SetPinMode(GPIOA, LL_GPIO_PIN_8, LL_GPIO_MODE_OUTPUT);
    LL_GPIO_SetPinSpeed(GPIOA, LL_GPIO_PIN_8, LL_GPIO_SPEED_FREQ_HIGH);
    LL_GPIO_ResetOutputPin(GPIOA, LL_GPIO_PIN_8);

    LL_USART_SetBaudRate(USART1, SystemCoreClock / 2, LL_USART_OVERSAMPLING_16, baudrate);
    LL_USART_SetDataWidth(USART1, LL_USART_DATAWIDTH_8B);
    LL_USART_SetStopBitsLength(USART1, LL_USART_STOPBITS_1);
    LL_USART_SetParity(USART1, LL_USART_PARITY_NONE);
    LL_USART_SetTransferDirection(USART1, LL_USART_DIRECTION_TX_RX);

    /* USART1 RX DMA: DMA2 Stream2 Ch4 */
    LL_DMA_DisableStream(DMA2, LL_DMA_STREAM_2);
    LL_DMA_ConfigTransfer(DMA2, LL_DMA_STREAM_2,
        LL_DMA_DIRECTION_PERIPH_TO_MEMORY |
        LL_DMA_PRIORITY_HIGH |
        LL_DMA_MODE_NORMAL |
        LL_DMA_MDATAALIGN_BYTE |
        LL_DMA_PDATAALIGN_BYTE |
        LL_DMA_MEMORY_INCREMENT |
        LL_DMA_PERIPH_NOINCREMENT);
    LL_DMA_SetChannelSelection(DMA2, LL_DMA_STREAM_2, LL_DMA_CHANNEL_4);
    LL_DMA_SetPeriphAddress(DMA2, LL_DMA_STREAM_2, (uint32_t)&USART1->DR);
    LL_DMA_SetMemoryAddress(DMA2, LL_DMA_STREAM_2, (uint32_t)s_rs_dma_buf);
    LL_DMA_SetDataLength(DMA2, LL_DMA_STREAM_2, BUF_SIZE);

    LL_USART_EnableDMAReq_RX(USART1);
    LL_USART_EnableIT_IDLE(USART1);
    NVIC_SetPriority(USART1_IRQn, 5);
    NVIC_EnableIRQ(USART1_IRQn);
    LL_DMA_EnableStream(DMA2, LL_DMA_STREAM_2);
    LL_USART_Enable(USART1);

    /* ── USART2 RX DMA: DMA1 Stream5 Ch4 ────────────────────────────── */
    LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_DMA1);

    LL_DMA_DisableStream(DMA1, LL_DMA_STREAM_5);
    LL_DMA_ConfigTransfer(DMA1, LL_DMA_STREAM_5,
        LL_DMA_DIRECTION_PERIPH_TO_MEMORY |
        LL_DMA_PRIORITY_HIGH |
        LL_DMA_MODE_NORMAL |
        LL_DMA_MDATAALIGN_BYTE |
        LL_DMA_PDATAALIGN_BYTE |
        LL_DMA_MEMORY_INCREMENT |
        LL_DMA_PERIPH_NOINCREMENT);
    LL_DMA_SetChannelSelection(DMA1, LL_DMA_STREAM_5, LL_DMA_CHANNEL_4);
    LL_DMA_SetPeriphAddress(DMA1, LL_DMA_STREAM_5, (uint32_t)&USART2->DR);
    LL_DMA_SetMemoryAddress(DMA1, LL_DMA_STREAM_5, (uint32_t)s_pc_dma_buf);
    LL_DMA_SetDataLength(DMA1, LL_DMA_STREAM_5, BUF_SIZE);

    LL_USART_EnableDMAReq_RX(USART2);
    LL_USART_EnableIT_IDLE(USART2);
    /* USART2 IRQ already enabled in uart_io.c — add IDLE handling there */
    LL_DMA_EnableStream(DMA1, LL_DMA_STREAM_5);
}

/* ── USART1 IDLE IRQ: RS-485 frame complete ──────────────────────────── */
void USART1_IRQHandler(void) {
    if (LL_USART_IsActiveFlag_IDLE(USART1)) {
        LL_USART_ClearFlag_IDLE(USART1);
        LL_DMA_DisableStream(DMA2, LL_DMA_STREAM_2);
        s_rs_frame_len = BUF_SIZE - LL_DMA_GetDataLength(DMA2, LL_DMA_STREAM_2);
        if (s_rs_frame_len > 0) {
            s_rs_frame_ready = true;
            s_rs485_frames++;
        }
        /* Reset DMA for next frame */
        LL_DMA_SetMemoryAddress(DMA2, LL_DMA_STREAM_2, (uint32_t)s_rs_dma_buf);
        LL_DMA_SetDataLength(DMA2, LL_DMA_STREAM_2, BUF_SIZE);
        LL_DMA_ClearFlag_TC2(DMA2);
        LL_DMA_ClearFlag_HT2(DMA2);
        LL_DMA_EnableStream(DMA2, LL_DMA_STREAM_2);
    }
    if (LL_USART_IsActiveFlag_ORE(USART1)) {
        LL_USART_ClearFlag_ORE(USART1);
    }
}

/* ── USART2 IDLE: called from uart_io.c USART2_IRQHandler ────────────── */
void modbus_bridge_usart2_idle(void) {
    LL_DMA_DisableStream(DMA1, LL_DMA_STREAM_5);
    s_pc_frame_len = BUF_SIZE - LL_DMA_GetDataLength(DMA1, LL_DMA_STREAM_5);
    if (s_pc_frame_len > 0) {
        s_pc_frame_ready = true;
        s_pc_frames++;
    }
    /* Reset DMA for next frame */
    LL_DMA_SetMemoryAddress(DMA1, LL_DMA_STREAM_5, (uint32_t)s_pc_dma_buf);
    LL_DMA_SetDataLength(DMA1, LL_DMA_STREAM_5, BUF_SIZE);
    LL_DMA_ClearFlag_TC5(DMA1);
    LL_DMA_ClearFlag_HT5(DMA1);
    LL_DMA_EnableStream(DMA1, LL_DMA_STREAM_5);
}

/* ── Baud rate table ──────────────────────────────────────────────────── */
static const uint32_t s_baud_table[] = { 9600, 19200, 38400, 57600, 115200, 230400, 460800 };
#define BAUD_COUNT 7
static uint8_t s_baud_idx = 4;

uint8_t modbus_bridge_get_baud_idx(void) { return s_baud_idx; }

static void set_usart1_baud(uint8_t idx) {
    if (idx >= BAUD_COUNT) return;
    s_baud_idx = idx;
    LL_USART_Disable(USART1);
    LL_USART_SetBaudRate(USART1, SystemCoreClock / 2, LL_USART_OVERSAMPLING_16, s_baud_table[idx]);
    LL_USART_Enable(USART1);
}

/* ── Relay task: forward complete frames ──────────────────────────────── */
void modbus_bridge_relay(void) {
    /* PC → RS-485: check for bridge config command (0xFE 0xFE) */
    if (s_pc_frame_ready) {
        if (s_pc_frame_len >= 4 && s_pc_dma_buf[0] == 0xFE && s_pc_dma_buf[1] == 0xFE) {
            uint8_t cmd = s_pc_dma_buf[2];
            uint8_t val = s_pc_dma_buf[3];
            if (cmd == 0x01) { set_usart1_baud(val); eeprom_write_u8(CFG_KEY_MB_BAUD, val); }
            if (cmd == 0x02) { can_bridge_set_baud(val); eeprom_write_u8(CFG_KEY_CAN_BAUD, val); }
            if (cmd == 0x10) {
                fault_entry_t faults[50];
                uint16_t fc = eeprom_read_faults(faults, 50);
                uint8_t pkt[4 + 50 * sizeof(fault_entry_t)];
                pkt[0] = 0xFE; pkt[1] = 0xFE; pkt[2] = 0x10; pkt[3] = (uint8_t)fc;
                memcpy(&pkt[4], faults, fc * sizeof(fault_entry_t));
                uart_write(pkt, 4 + fc * sizeof(fault_entry_t));
            }
            s_pc_frame_ready = false;
        }
    }

    /* PC → RS-485: forward complete frame */
    if (s_pc_frame_ready) {
        rs485_tx(s_pc_dma_buf, s_pc_frame_len);
        s_pc_frame_ready = false;
    }

    /* RS-485 → PC: forward complete frame */
    if (s_rs_frame_ready) {
        uart_write(s_rs_dma_buf, s_rs_frame_len);
        s_rs_frame_ready = false;
    }
}

/* ── Mode switch ──────────────────────────────────────────────────────── */
static volatile bool s_gateway_mode = false;

void modbus_bridge_set_mode(bool gateway) {
    s_gateway_mode = gateway;
    /* Re-enable DMA RX for USART2 when switching back to relay */
    if (!gateway) {
        LL_DMA_DisableStream(DMA1, LL_DMA_STREAM_5);
        LL_DMA_SetMemoryAddress(DMA1, LL_DMA_STREAM_5, (uint32_t)s_pc_dma_buf);
        LL_DMA_SetDataLength(DMA1, LL_DMA_STREAM_5, BUF_SIZE);
        LL_DMA_ClearFlag_TC5(DMA1);
        LL_DMA_EnableStream(DMA1, LL_DMA_STREAM_5);
    }
}

/* ── Gateway mode: protocol-based 0x03/0x04 ──────────────────────────── */
void modbus_bridge_poll(void) {
    /* Handle PC frame received via DMA+IDLE (same buffer as relay) */
    if (s_pc_frame_ready) {
        uint8_t *frame = s_pc_dma_buf;
        uint16_t len = s_pc_frame_len;

        /* Bridge config: [0xFE, 0xFE, cmd, val] */
        if (len >= 3 && frame[0] == 0xFE && frame[1] == 0xFE) {
            uint8_t cmd = frame[2];
            uint8_t val = len >= 4 ? frame[3] : 0;
            if (cmd == 0x01) { set_usart1_baud(val); eeprom_write_u8(CFG_KEY_MB_BAUD, val); }
            if (cmd == 0x02) { can_bridge_set_baud(val); eeprom_write_u8(CFG_KEY_CAN_BAUD, val); }
            if (cmd == 0x10) {
                fault_entry_t faults[50];
                uint16_t fc = eeprom_read_faults(faults, 50);
                uint8_t pkt[4 + 50 * sizeof(fault_entry_t)];
                pkt[0] = 0xFE; pkt[1] = 0xFE; pkt[2] = 0x10; pkt[3] = (uint8_t)fc;
                memcpy(&pkt[4], faults, fc * sizeof(fault_entry_t));
                uart_write(pkt, 4 + fc * sizeof(fault_entry_t));
            }
            s_pc_frame_ready = false;
            return;
        }

        if (len >= 2 && frame[0] == 0x03) {
            /* 0x03 = Modbus TX pass-through: [0x03, len, raw...] */
            uint8_t mb_len = frame[1];
            if (mb_len <= len - 2) {
                rs485_tx(&frame[2], mb_len);
                s_pc_frames++;
            }
        }
        s_pc_frame_ready = false;
    }

    /* Forward RS-485 response as [0x04, len, raw...] */
    if (s_rs_frame_ready) {
        uint8_t pkt[2 + BUF_SIZE];
        pkt[0] = 0x04;
        pkt[1] = (uint8_t)s_rs_frame_len;
        memcpy(&pkt[2], s_rs_dma_buf, s_rs_frame_len);
        uart_write(pkt, 2 + s_rs_frame_len);
        s_rs_frame_ready = false;
    }
}

void modbus_bridge_send_raw(const uint8_t *data, uint16_t len) {
    rs485_tx(data, len);
}
