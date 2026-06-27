# STM32-CAN-Bridge

Nucleo-F446RE triple protocol gateway: UART ↔ CAN ↔ Modbus RS-485

## Build

```bash
# Ctrl+Shift+B ใน VS Code = build + flash ทีเดียวจบ
# หรือ manual:
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/gnu-tools-for-stm32.cmake -DCMAKE_BUILD_TYPE=Debug
cd build/Debug && ninja
```

Toolchain: `C:\ST\STM32CubeCLT_1.21.0\GNU-tools-for-STM32\bin`
Flash: `STM32_Programmer_CLI -c port=SWD -w build/Debug/STM32-CAN-Bridge.bin 0x08000000 -v -rst`

## Pin Map

| Function | STM32 | Arduino | Morpho |
|----------|-------|---------|--------|
| USART2 TX (VCP) | PA2 | D1 | CN10-35 | ต่อในตัว |
| USART2 RX (VCP) | PA3 | D0 | CN10-37 | ต่อในตัว |
| I2C1 SCL (OLED) | PB8 | **D15** | CN10-3 |
| I2C1 SDA (OLED) | PB9 | **D14** | CN10-5 |
| CAN1 RX | PA11 | — | CN10-14 |
| CAN1 TX | PA12 | — | CN10-12 |
| Modbus TX | PA9 | D8 | CN7-1 |
| Modbus RX | PA10 | D2 | CN7-33 |
| RS-485 DE | PA8 | D7 | CN10-8 |
| LED | PA5 | D13 | CN10-11 |

## Bridge Protocol

```
0x01 = CAN TX    (PC → CAN bus)
0x02 = CAN RX    (CAN bus → PC)
0x03 = Modbus TX (PC → RS-485 pass-through)
0x04 = Modbus RX (RS-485 response → PC)
```

Compatible with PicoCANBridge (same 0x01/0x02 format).
SonoPilotModbus GUI: ติ๊ก "Bridge" checkbox เพื่อใช้ผ่าน STM32.

## Architecture

- **CAN:** HAL_CAN HW peripheral (ไม่ใช่ PIO — ไม่มีปัญหา audio stutter)
- **Modbus:** USART1 + RS-485 DE, Modbus RTU master, CRC16 SW
- **OLED:** SSD1306 128×64 I2C1 DMA double buffer (เหมือน Pico)
- **RTOS:** FreeRTOS, single task bridge loop

## Port to F103 (Blue Pill)

- เปลี่ยน startup + linker script
- CAN1: PA11/PA12 (default, ไม่ต้อง remap)
- APB1 = 36MHz → CAN prescaler: 18/9/4/2
- CAN + USB share SRAM — config filter bank offset
- HAL API เหมือนกัน

## Critical Rules

- VS Code build/flash ต้องกดปุ่มเดียวจบ (Ctrl+Shift+B) ห้ามแยก task
- OLED D14/D15 ตามที่พิมพ์บนบอร์ด Nucleo
- CAN ใช้ PA11/PA12 (Morpho) ไม่ใช่ D14/D15 เพราะชนกับ I2C
- Nucleo ไม่มี ICSP header — ใช้ W5500 module ต่อ SPI ตรง ไม่ใช่ Arduino Shield

## Related Repos

| Repo | Purpose |
|------|---------|
| rp2350-mp3-player | SonoPilot firmware (Pico 2) |
| SonoPilotModbus | PC GUI (Modbus + Bridge mode) |
| PicoCANBridge | USB HID-CAN bridge (Pico) |
| PicoCANGui | CAN GUI (PC) |
