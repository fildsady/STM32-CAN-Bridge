#pragma once
#include <stdint.h>
#include <stdbool.h>

#define BRIDGE_CMD_CAN_TX   0x01
#define BRIDGE_CMD_CAN_RX   0x02

#define CAN_BAUD_20K    0
#define CAN_BAUD_50K    1
#define CAN_BAUD_125K   2
#define CAN_BAUD_250K   3
#define CAN_BAUD_500K   4
#define CAN_BAUD_1M     5

void can_bridge_init(uint8_t baud_idx);
void can_bridge_poll(void);
bool can_bridge_send(uint32_t id, const uint8_t *data, uint8_t dlc);
void can_bridge_set_baud(uint8_t idx);
uint8_t can_bridge_get_baud_idx(void);

uint32_t can_bridge_tx_count(void);
uint32_t can_bridge_rx_count(void);
uint32_t can_bridge_err_count(void);
