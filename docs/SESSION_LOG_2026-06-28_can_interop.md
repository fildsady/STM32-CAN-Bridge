# Session Log: CAN bxCAN↔can2040 Interop Fix

**วันที่:** 2026-06-28
**Branch:** `feature/gateway`

## สรุป

แก้ปัญหา STM32 bxCAN สื่อสารกับ Pico can2040 ไม่ได้ — root cause คือ sample point ต่างกัน 12% พร้อมแก้ MB TX counter นับผิด และขยาย CAN RX ring buffer

## Root Cause: Sample Point Mismatch

| พารามิเตอร์ | can2040 (Pico) | bxCAN (STM32) เดิม | bxCAN ใหม่ |
|-------------|---------------|-------------------|-----------|
| Sample point | ≈75% | 86.7% | 73.3% |
| BS1 | — (PIO 32 clk) | 12 TQ | 10 TQ |
| BS2 | — | 2 TQ | 4 TQ |

### การวิเคราะห์ (ตามลำดับ)

1. **CAN loopback test** — เพิ่ม `CAN_BTR_LBKM` → TX+RX ขึ้น, LEC=0 → peripheral ปกติ
2. **Transceiver bit-bang test** — `can_bridge_loopback_test()` PA12↔PA11 → 0x03 PASS
3. **ESR/LEC display บน OLED** — TEC สูง, LEC=5 (Bit Dominant) ตอนไม่มี node = Bus Off recovery ปกติ
4. **แก้ BTR** — BS1=10, BS2=4 → sample point 73.3% ใกล้ can2040 → **สื่อสารได้**

### วิธีแก้

```c
// เดิม: SP=86.7%
CAN1->BTR = ((1-1)<<24) | ((2-1)<<20) | ((12-1)<<16) | (prescaler-1);

// ใหม่: SP=73.3%
CAN1->BTR = ((1-1)<<24) | ((4-1)<<20) | ((10-1)<<16) | (prescaler-1);
```

แก้ทั้ง `can_bridge_init()` และ `can_bridge_set_baud()` — prescaler ไม่เปลี่ยน baud rate เท่าเดิม

## แก้ MB TX Counter นับผิด

`s_pc_frames++` อยู่ใน USART2 IDLE IRQ → นับทุก frame จาก PC รวม CAN TX (0x01) → CAN TX กับ MB TX ขึ้นพร้อมกัน

แก้: ย้าย `s_pc_frames++` จาก IDLE IRQ ไปนับเฉพาะใน `modbus_bridge_poll()` ตอน forward 0x03

## ขยาย CAN RX Ring Buffer

16 → 64 slots — รองรับ 16 node ส่ง HB พร้อมกัน (48 frames burst)

## ไฟล์ที่แก้

- `Src/can_bridge.c` — BTR SP=73.3%, loopback test, ring buffer 64, `can_bridge_esr()`
- `Src/main.c` — OLED CAN/MB counters + ESR compact + transceiver test ตอน boot
- `Inc/can_bridge.h` — เพิ่ม `can_bridge_esr()`, `can_bridge_loopback_test()`
- `Src/modbus_bridge.c` — แก้ `s_pc_frames` นับเฉพาะ Modbus

## บทเรียน

- **Sample point ต้องตรงกับ can2040 (~75%)** — bxCAN default 87.5% ใช้ไม่ได้กับ software CAN
- **LEC=5 ตอนไม่มี node ≠ bug** — Bus Off recovery cycle ปกติของ CAN ไม่มี ACK
- **Counter ต้องนับถูก layer** — UART IDLE = physical frame, ไม่ใช่ protocol frame
