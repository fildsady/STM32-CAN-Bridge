/*
 * can_bridge.c — CAN1 ↔ UART bridge (transparent pass-through)
 *
 * STM32F446RE CAN1: PB8=RX, PB9=TX (remap, avoid USB conflict on PA11/PA12)
 * Transceiver: SN65HVD230 or TJA1050
 *
 * Protocol (same as PicoCANBridge):
 *   PC → Bridge: [0x01, ID_H, ID_L, DLC, data[0..7]]
 *   Bridge → PC: [0x02, ID_H, ID_L, DLC, data[0..7]]
 */
#include "can_bridge.h"
#include "uart_io.h"
#include "stm32f4xx.h"
#include "stm32f4xx_ll_bus.h"
#include "stm32f4xx_ll_gpio.h"
#include <string.h>

static CAN_HandleTypeDef s_hcan;
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

/* Baud rate prescaler table (APB1 = 45MHz on F446RE @ 180MHz) */
/* CAN bit time = prescaler * (1 + BS1 + BS2) / APB1_CLK            */
/* BS1=12, BS2=2 → 15 TQ per bit                                     */
static const struct { uint16_t prescaler; uint32_t baud; } s_baud_table[] = {
    { 24, 125000  },  /* 45MHz / 24 / 15 = 125k  */
    { 12, 250000  },  /* 45MHz / 12 / 15 = 250k  */
    {  6, 500000  },  /* 45MHz /  6 / 15 = 500k  */
    {  3, 1000000 },  /* 45MHz /  3 / 15 = 1M    */
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

    __HAL_RCC_CAN1_CLK_ENABLE();

    s_hcan.Instance = CAN1;
    s_hcan.Init.Prescaler = s_baud_table[baud_idx].prescaler;
    s_hcan.Init.Mode = CAN_MODE_NORMAL;
    s_hcan.Init.SyncJumpWidth = CAN_SJW_1TQ;
    s_hcan.Init.TimeSeg1 = CAN_BS1_12TQ;
    s_hcan.Init.TimeSeg2 = CAN_BS2_2TQ;
    s_hcan.Init.TimeTriggeredMode = DISABLE;
    s_hcan.Init.AutoBusOff = ENABLE;
    s_hcan.Init.AutoWakeUp = ENABLE;
    s_hcan.Init.AutoRetransmission = DISABLE;
    s_hcan.Init.ReceiveFifoLocked = DISABLE;
    s_hcan.Init.TransmitFifoPriority = DISABLE;
    HAL_CAN_Init(&s_hcan);

    /* Accept all messages — no filter */
    CAN_FilterTypeDef filter = {0};
    filter.FilterBank = 0;
    filter.FilterMode = CAN_FILTERMODE_IDMASK;
    filter.FilterScale = CAN_FILTERSCALE_32BIT;
    filter.FilterIdHigh = 0;
    filter.FilterIdLow = 0;
    filter.FilterMaskIdHigh = 0;
    filter.FilterMaskIdLow = 0;
    filter.FilterFIFOAssignment = CAN_RX_FIFO0;
    filter.FilterActivation = ENABLE;
    HAL_CAN_ConfigFilter(&s_hcan, &filter);

    /* Enable RX interrupt */
    HAL_CAN_ActivateNotification(&s_hcan, CAN_IT_RX_FIFO0_MSG_PENDING);
    NVIC_SetPriority(CAN1_RX0_IRQn, 5);
    NVIC_EnableIRQ(CAN1_RX0_IRQn);

    HAL_CAN_Start(&s_hcan);
}

/* CAN RX ISR → ring buffer */
void CAN1_RX0_IRQHandler(void) {
    CAN_RxHeaderTypeDef hdr;
    uint8_t data[8];

    while (HAL_CAN_GetRxFifoFillLevel(&s_hcan, CAN_RX_FIFO0) > 0) {
        if (HAL_CAN_GetRxMessage(&s_hcan, CAN_RX_FIFO0, &hdr, data) == HAL_OK) {
            uint8_t next = (s_rx_ring_head + 1) % RX_RING_SIZE;
            if (next != s_rx_ring_tail) {
                s_rx_ring[s_rx_ring_head].id = hdr.StdId;
                s_rx_ring[s_rx_ring_head].dlc = (uint8_t)hdr.DLC;
                memcpy((void *)s_rx_ring[s_rx_ring_head].data, data, hdr.DLC);
                s_rx_ring_head = next;
            }
            s_rx_count++;
        }
    }
}

bool can_bridge_send(uint32_t id, const uint8_t *data, uint8_t dlc) {
    CAN_TxHeaderTypeDef hdr = {0};
    hdr.StdId = id & 0x7FF;
    hdr.IDE = CAN_ID_STD;
    hdr.RTR = CAN_RTR_DATA;
    hdr.DLC = dlc > 8 ? 8 : dlc;

    uint32_t mailbox;
    if (HAL_CAN_AddTxMessage(&s_hcan, &hdr, (uint8_t *)data, &mailbox) == HAL_OK) {
        s_tx_count++;
        return true;
    }
    s_err_count++;
    return false;
}

/* Process UART RX → CAN TX, CAN RX ring → UART TX */
void can_bridge_poll(void) {
    /* UART → CAN: parse [0x01, ID_H, ID_L, DLC, data...] */
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

    /* CAN RX ring → UART: send [0x02, ID_H, ID_L, DLC, data...] */
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
