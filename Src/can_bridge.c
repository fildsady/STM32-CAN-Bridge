/*
 * can_bridge.c — CAN1 ↔ UART bridge (register-level, no HAL)
 *
 * STM32F446RE CAN1: PA11=RX, PA12=TX
 * Transceiver: SN65HVD230 or TJA1050
 */
#include "can_bridge.h"
#include "uart_io.h"
#include "stm32f4xx.h"
#include "stm32f4xx_ll_bus.h"
#include "stm32f4xx_ll_gpio.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>

static volatile uint32_t s_tx_count = 0;
static volatile uint32_t s_rx_count = 0;
static volatile uint32_t s_err_count = 0;

/* RX ring buffer (ISR → task) */
#define RX_RING_SIZE 16
typedef struct {
    uint32_t id;
    uint8_t  dlc;
    uint8_t  data[8];
} can_frame_t;

static volatile can_frame_t s_rx_ring[RX_RING_SIZE];
static volatile uint8_t s_rx_ring_head = 0;
static volatile uint8_t s_rx_ring_tail = 0;

/* Baud rate prescaler table (APB1 = 45MHz, 15 TQ per bit) */
static const struct { uint16_t prescaler; uint32_t baud; } s_baud_table[] = {
    { 24, 125000  },
    { 12, 250000  },
    {  6, 500000  },
    {  3, 1000000 },
};
#define BAUD_COUNT (sizeof(s_baud_table) / sizeof(s_baud_table[0]))

static void can_gpio_init(void) {
    LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_GPIOA);

    /* PA11 = CAN1_RX, PA12 = CAN1_TX → AF9 */
    LL_GPIO_SetPinMode(GPIOA, LL_GPIO_PIN_11, LL_GPIO_MODE_ALTERNATE);
    LL_GPIO_SetAFPin_8_15(GPIOA, LL_GPIO_PIN_11, LL_GPIO_AF_9);
    LL_GPIO_SetPinPull(GPIOA, LL_GPIO_PIN_11, LL_GPIO_PULL_UP);

    LL_GPIO_SetPinMode(GPIOA, LL_GPIO_PIN_12, LL_GPIO_MODE_ALTERNATE);
    LL_GPIO_SetAFPin_8_15(GPIOA, LL_GPIO_PIN_12, LL_GPIO_AF_9);
    LL_GPIO_SetPinSpeed(GPIOA, LL_GPIO_PIN_12, LL_GPIO_SPEED_FREQ_HIGH);
}

void can_bridge_init(uint8_t baud_idx) {
    if (baud_idx >= BAUD_COUNT) baud_idx = 0;

    can_gpio_init();

    /* Enable CAN1 clock */
    RCC->APB1ENR |= RCC_APB1ENR_CAN1EN;

    /* Enter init mode */
    CAN1->MCR |= CAN_MCR_INRQ;
    while (!(CAN1->MSR & CAN_MSR_INAK)) {}

    /* Exit sleep */
    CAN1->MCR &= ~CAN_MCR_SLEEP;

    /* Configure: no auto-retransmit, auto bus-off recovery, auto wakeup */
    CAN1->MCR |= CAN_MCR_NART | CAN_MCR_ABOM | CAN_MCR_AWUM;

    /* Bit timing: BS1=12TQ, BS2=2TQ, SJW=1TQ */
    CAN1->BTR = ((1 - 1) << 24) |    /* SJW = 1TQ */
                ((2 - 1) << 20) |    /* BS2 = 2TQ */
                ((12 - 1) << 16) |   /* BS1 = 12TQ */
                (s_baud_table[baud_idx].prescaler - 1);

    /* Accept all — filter bank 0, mask mode, all pass */
    CAN1->FMR |= CAN_FMR_FINIT;
    CAN1->FA1R &= ~(1 << 0);
    CAN1->sFilterRegister[0].FR1 = 0;
    CAN1->sFilterRegister[0].FR2 = 0;
    CAN1->FM1R &= ~(1 << 0);    /* mask mode */
    CAN1->FS1R |= (1 << 0);     /* 32-bit scale */
    CAN1->FFA1R &= ~(1 << 0);   /* assign to FIFO0 */
    CAN1->FA1R |= (1 << 0);     /* activate filter 0 */
    CAN1->FMR &= ~CAN_FMR_FINIT;

    /* Enable FIFO0 message pending interrupt */
    CAN1->IER |= CAN_IER_FMPIE0;
    NVIC_SetPriority(CAN1_RX0_IRQn, 5);
    NVIC_EnableIRQ(CAN1_RX0_IRQn);

    /* Leave init mode → normal */
    CAN1->MCR &= ~CAN_MCR_INRQ;
    while (CAN1->MSR & CAN_MSR_INAK) {}
}

/* CAN RX ISR → ring buffer */
void CAN1_RX0_IRQHandler(void) {
    while (CAN1->RF0R & CAN_RF0R_FMP0) {
        uint8_t next = (s_rx_ring_head + 1) % RX_RING_SIZE;
        if (next != s_rx_ring_tail) {
            uint32_t rir = CAN1->sFIFOMailBox[0].RIR;
            s_rx_ring[s_rx_ring_head].id = (rir >> 21) & 0x7FF;
            s_rx_ring[s_rx_ring_head].dlc = CAN1->sFIFOMailBox[0].RDTR & 0x0F;
            if (s_rx_ring[s_rx_ring_head].dlc > 8) s_rx_ring[s_rx_ring_head].dlc = 8;

            uint32_t rdlr = CAN1->sFIFOMailBox[0].RDLR;
            uint32_t rdhr = CAN1->sFIFOMailBox[0].RDHR;
            s_rx_ring[s_rx_ring_head].data[0] = (uint8_t)(rdlr);
            s_rx_ring[s_rx_ring_head].data[1] = (uint8_t)(rdlr >> 8);
            s_rx_ring[s_rx_ring_head].data[2] = (uint8_t)(rdlr >> 16);
            s_rx_ring[s_rx_ring_head].data[3] = (uint8_t)(rdlr >> 24);
            s_rx_ring[s_rx_ring_head].data[4] = (uint8_t)(rdhr);
            s_rx_ring[s_rx_ring_head].data[5] = (uint8_t)(rdhr >> 8);
            s_rx_ring[s_rx_ring_head].data[6] = (uint8_t)(rdhr >> 16);
            s_rx_ring[s_rx_ring_head].data[7] = (uint8_t)(rdhr >> 24);

            s_rx_ring_head = next;
            s_rx_count++;
        }
        /* Release FIFO */
        CAN1->RF0R |= CAN_RF0R_RFOM0;
    }
}

bool can_bridge_send(uint32_t id, const uint8_t *data, uint8_t dlc) {
    if (dlc > 8) dlc = 8;

    /* Find empty TX mailbox */
    uint32_t tsr = CAN1->TSR;
    uint8_t mb;
    if (tsr & CAN_TSR_TME0) mb = 0;
    else if (tsr & CAN_TSR_TME1) mb = 1;
    else if (tsr & CAN_TSR_TME2) mb = 2;
    else { s_err_count++; return false; }

    /* Set ID (standard, data frame) */
    CAN1->sTxMailBox[mb].TIR = ((id & 0x7FF) << 21);

    /* Set DLC */
    CAN1->sTxMailBox[mb].TDTR = dlc;

    /* Set data */
    CAN1->sTxMailBox[mb].TDLR = (uint32_t)data[0] |
                                 ((uint32_t)data[1] << 8) |
                                 ((uint32_t)data[2] << 16) |
                                 ((uint32_t)data[3] << 24);
    CAN1->sTxMailBox[mb].TDHR = (uint32_t)data[4] |
                                 ((uint32_t)data[5] << 8) |
                                 ((uint32_t)data[6] << 16) |
                                 ((uint32_t)data[7] << 24);

    /* Request transmit */
    CAN1->sTxMailBox[mb].TIR |= CAN_TI0R_TXRQ;
    s_tx_count++;
    return true;
}

/* UART → CAN, CAN RX ring → UART */
void can_bridge_poll(void) {
    static uint8_t uart_buf[80];
    static uint8_t uart_pos = 0;

    while (uart_available() > 0) {
        uint8_t c = uart_read_byte();

        if (uart_pos == 0 && c != BRIDGE_CMD_CAN_TX && c != 0x03) continue;

        /* Modbus pass-through: [0x03, len, raw_frame...] */
        if (uart_pos == 0 && c == 0x03) {
            uart_buf[uart_pos++] = c;
            continue;
        }
        if (uart_buf[0] == 0x03) {
            uart_buf[uart_pos++] = c;
            if (uart_pos >= 2) {
                uint8_t mb_len = uart_buf[1];
                if (mb_len > 64) { uart_pos = 0; continue; }
                if (uart_pos >= (uint8_t)(2 + mb_len)) {
                    extern void modbus_bridge_send_raw(const uint8_t *data, uint16_t len);
                    modbus_bridge_send_raw(&uart_buf[2], mb_len);
                    uart_pos = 0;
                }
            }
            if (uart_pos >= sizeof(uart_buf)) uart_pos = 0;
            continue;
        }

        uart_buf[uart_pos++] = c;

        if (uart_pos >= 4) {
            uint8_t dlc = uart_buf[3];
            if (dlc > 8) { uart_pos = 0; continue; }
            if (uart_pos >= (uint8_t)(4 + dlc)) {
                uint32_t id = ((uint32_t)uart_buf[1] << 8) | uart_buf[2];
                can_bridge_send(id, &uart_buf[4], dlc);
                uart_pos = 0;
            }
        }
        if (uart_pos >= sizeof(uart_buf)) uart_pos = 0;
    }

    /* CAN RX ring → UART */
    while (s_rx_ring_tail != s_rx_ring_head) {
        volatile can_frame_t *f = &s_rx_ring[s_rx_ring_tail];
        uint8_t pkt[12];
        pkt[0] = BRIDGE_CMD_CAN_RX;
        pkt[1] = (uint8_t)(f->id >> 8);
        pkt[2] = (uint8_t)(f->id & 0xFF);
        pkt[3] = f->dlc;
        memcpy(&pkt[4], (const void *)f->data, f->dlc);
        uart_write(pkt, 4 + f->dlc);
        s_rx_ring_tail = (s_rx_ring_tail + 1) % RX_RING_SIZE;
    }
}

uint32_t can_bridge_tx_count(void)  { return s_tx_count; }
uint32_t can_bridge_rx_count(void)  { return s_rx_count; }
uint32_t can_bridge_err_count(void) { return s_err_count; }
