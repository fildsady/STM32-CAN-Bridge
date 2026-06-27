#pragma once
#include <stdint.h>
#include <stdbool.h>

/* UART-CAN Bridge protocol (same as PicoCANBridge):
 * PC → Bridge: [0x01, ID_H, ID_L, DLC, data...]  = send CAN frame
 * Bridge → PC: [0x02, ID_H, ID_L, DLC, data...]  = received CAN frame
 */
#define BRIDGE_CMD_CAN_TX   0x01
#define BRIDGE_CMD_CAN_RX   0x02

/* CAN config */
#define CAN_BAUD_125K   0
#define CAN_BAUD_250K   1
#define CAN_BAUD_500K   2
#define CAN_BAUD_1M     3

void can_bridge_init(uint8_t baud_idx);
void can_bridge_poll(void);
bool can_bridge_send(uint32_t id, const uint8_t *data, uint8_t dlc);

/* Stats */
uint32_t can_bridge_tx_count(void);
uint32_t can_bridge_rx_count(void);
uint32_t can_bridge_err_count(void);
