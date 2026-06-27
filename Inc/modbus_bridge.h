#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Modbus RTU Master via USART1 + RS-485 DE
 * PA9=TX, PA10=RX, PA8=DE
 *
 * Bridge protocol (PC → STM32):
 *   0x03 = Modbus TX: [0x03, len, raw_modbus_frame...]
 *   0x04 = Modbus RX: [0x04, len, raw_modbus_frame...]
 *
 * CAN ↔ Modbus translation:
 *   CAN CMD 0x100+N → Modbus FC06 write to slave N
 *   Modbus response  → CAN status frame 0x300+N
 */

#define BRIDGE_CMD_MODBUS_TX  0x03
#define BRIDGE_CMD_MODBUS_RX  0x04

#define MODBUS_DE_PIN         LL_GPIO_PIN_8
#define MODBUS_DE_PORT        GPIOA

void modbus_bridge_init(uint32_t baudrate);
void modbus_bridge_poll(void);
void modbus_bridge_relay(void);
void modbus_bridge_set_mode(bool gateway);
uint32_t modbus_bridge_pc_count(void);
uint32_t modbus_bridge_rs485_count(void);

/* Send raw Modbus frame (from PC pass-through) */
void modbus_bridge_send_raw(const uint8_t *data, uint16_t len);

/* Translate CAN command → Modbus write to slave */
bool modbus_bridge_can_to_modbus(uint32_t can_id, const uint8_t *data, uint8_t dlc);

/* Read Modbus slave → return as CAN-compatible status */
bool modbus_bridge_poll_slave(uint8_t slave_id, uint8_t *out_data, uint8_t *out_len);
