# STM32-CAN-Bridge

UART ↔ CAN transparent gateway on Nucleo-F446RE with SSD1306 OLED display.
Protocol compatible with PicoCANBridge / PicoCANGui.

## Hardware

- **Board:** Nucleo-64 STM32F446RE (Cortex-M4 180MHz)
- **CAN:** CAN1 HW peripheral + SN65HVD230 transceiver
- **UART:** USART2 via ST-Link VCP (USB COM port ในตัว)
- **OLED:** SSD1306 128×64 I2C, DMA double buffer
- **LED:** PA5 — activity blink

## Pin Map

### STM32 → Arduino Header (Nucleo-F446RE)

| Function | STM32 Pin | Arduino Pin | Morpho Pin | หมายเหตุ |
|----------|-----------|-------------|------------|----------|
| USART2 TX | PA2 | **A7** (D1) | CN10-35 | ST-Link VCP (ต่อให้แล้ว) |
| USART2 RX | PA3 | **A2** (D0) | CN10-37 | ST-Link VCP (ต่อให้แล้ว) |
| CAN1 RX | PB8 | **D15** | CN10-3 | ต่อ SN65HVD230 RXD |
| CAN1 TX | PB9 | **D14** | CN10-5 | ต่อ SN65HVD230 TXD |
| I2C1 SCL | PB6 | **D10** | CN10-17 | ต่อ OLED SCL |
| I2C1 SDA | PB7 | **D9** (ไม่มีบน Arduino header) | CN7-21 | ต่อ OLED SDA |
| LED | PA5 | **D13** | CN10-11 | LED บนบอร์ด |
| User Button | PC13 | — | CN7-23 | กดติดดิน |

### Morpho Connector (pin ที่ไม่อยู่บน Arduino header)

```
CN7 (ซ้าย)                    CN10 (ขวา)
 Pin 21 = PB7 (I2C1_SDA)       Pin 3  = PB8 (CAN1_RX)
                                Pin 5  = PB9 (CAN1_TX)
                                Pin 17 = PB6 (I2C1_SCL)
```

**หมายเหตุ:** PB7 (I2C1_SDA) ไม่อยู่บน Arduino header — ต้องใช้ Morpho connector CN7 pin 21

## Wiring Diagram

```
Nucleo F446RE              SN65HVD230            SSD1306 OLED
  D14/PB9 (CAN1_TX) ────── TXD                 
  D15/PB8 (CAN1_RX) ────── RXD                 
  3V3 ─────────────────── VCC              ──── VCC
  GND ─────────────────── GND              ──── GND
                           CANH ──── Bus
                           CANL ──── Bus
  D10/PB6 (I2C1_SCL) ─────────────────────────── SCL
  CN7-21/PB7 (I2C1_SDA) ──────────────────────── SDA

  [USB] ST-Link ←→ PC (COM port อัตโนมัติ)
  [D13/PA5] LED กระพริบ = bridge ทำงาน
```

## Protocol

Same as PicoCANBridge — compatible with PicoCANGui.

```
PC → Bridge: [0x01, ID_H, ID_L, DLC, data[0..7]]   CAN TX
Bridge → PC: [0x02, ID_H, ID_L, DLC, data[0..7]]   CAN RX
```

Transport: UART 115200 8N1 ผ่าน ST-Link VCP (USB COM port)

## CAN Baud Rate

| Index | Baud | Prescaler | หมายเหตุ |
|-------|------|-----------|----------|
| 0 | 125k | 24 | default, สายยาว 200m+ |
| 1 | 250k | 12 | |
| 2 | 500k | 6 | |
| 3 | 1M | 3 | สายสั้น < 10m |

APB1 = 45MHz, BS1=12TQ, BS2=2TQ, SJW=1TQ (15 TQ per bit)

## OLED Display

- SSD1306 128×64, I2C1 400kHz
- DMA double buffer (เหมือน Pico version)
- Draw to back buffer → swap → DMA transfer (~23ms)
- FreeRTOS task draws freely ระหว่าง DMA ทำงาน

## Architecture

```
                    FreeRTOS
                   ┌─────────────────────────┐
                   │  task_bridge             │
  ST-Link VCP ◄────┤  UART RX → CAN TX       │
  (USB COM)   ────►│  CAN RX ring → UART TX  │
                   │  OLED update @ 30fps     │
                   └─────────────────────────┘
                          │          │
                     CAN1 HW    I2C1+DMA
                     (PB8/PB9)  (PB6/PB7)
                          │          │
                    SN65HVD230   SSD1306
                          │
                     CAN Bus
```

## Build

```bash
cmake -B build -G Ninja
cd build && ninja
```

หรือเปิดใน VS Code + STM32 Extension

## Flash

ST-Link on Nucleo — ลากไฟล์ .bin ไปที่ NODE_F446RE drive:

```bash
# หรือใช้ st-flash
st-flash write build/STM32-CAN-Bridge.bin 0x08000000

# หรือใช้ OpenOCD
openocd -f interface/stlink.cfg -f target/stm32f4x.cfg \
  -c "program build/STM32-CAN-Bridge.bin 0x08000000 verify reset exit"
```

## Port to STM32F103 (Blue Pill)

| เปลี่ยน | F446RE | F103C8 |
|---------|--------|--------|
| Startup | startup_stm32f446xx.S | startup_stm32f103xb.S |
| Linker | stm32f446xe_flash.ld | stm32f103c8_flash.ld |
| Clock | 180MHz (HSE 8MHz) | 72MHz (HSE 8MHz) |
| APB1 | 45MHz | 36MHz |
| CAN prescaler | 24/12/6/3 | 18/9/4/2 (adjust for 36MHz) |
| CAN pins | PB8/PB9 (remap) | PA11/PA12 (default) |
| I2C pins | PB6/PB7 | PB6/PB7 (เหมือนกัน) |
| CAN+USB | ใช้พร้อมกันได้ | share SRAM — config filter bank offset |
| HAL API | เหมือนกัน | เหมือนกัน |
