# Session Log — 28 มิถุนายน 2026 (Part 2)

## สรุป

ต่อจาก Part 1 — ทำ STM32-CAN-Bridge firmware + STM32-Gateway-GUI + SonoPilotModbus แก้ bug

---

## 1. STM32-CAN-Bridge — Gateway Mode + Flash Config

### Dual Mode (feature/gateway)
- ปุ่ม PC13 สลับ **RELAY ↔ GATEWAY**
- Boot default = GATEWAY
- OLED แสดง mode + CAN/MB counters + baud

### EEPROM Emulation (flash sector 3)
- Key-value store บน internal flash sector 3 (0x0800C000, 16KB)
- **ปัญหา:** sector ไม่ blank (0x00 แทน 0xFF) → flash write ไม่เข้า
- **แก้:** eeprom_init ตรวจ first byte ถ้าไม่ blank + ไม่ valid → erase sector ก่อน
- **ปัญหา:** interrupt กวน flash write → data corrupt
- **แก้:** `__disable_irq()` ระหว่าง flash unlock/write/lock
- **ปัญหา:** debug test code erase sector 3 ทุก boot
- **แก้:** ลบ debug code ออก
- **ปัญหา:** MB baud ไม่จำ (CAN จำได้)
- **สาเหตุ:** `modbus_bridge_init()` ไม่ set `s_baud_idx` ตอน boot
- **แก้:** boot เรียก `modbus_bridge_set_baud(saved_idx)` หลัง init

### Config ที่ save ได้
- MB baud index (0-6) → key 0x01
- CAN baud index (0-5) → key 0x02

### Fault Log
- HardFault, StackOverflow → save PC/LR/CFSR/uptime ลง flash
- Test Fault (magic 0xFE 0x11) → เขียน fake fault ทดสอบ
- Export (magic 0xFE 0x10) → bridge ส่ง fault entries กลับ PC

### CAN Baud Table ขยาย
- 20k / 50k / 125k / 250k / 500k / 1M (6 ระดับ)

### PC→Bridge Baud
- เปลี่ยนจาก 115200 → 460800 → **921600**
- ST-Link VCP รองรับ (ไม่ใช่ USB ตรง มี F103 ขั้นกลาง)

### Magic Header Protocol (0xFE 0xFE)
| Cmd | Data | ทำอะไร |
|-----|------|--------|
| 0x01 | baud_idx | เปลี่ยน MB baud + save flash |
| 0x02 | baud_idx | เปลี่ยน CAN baud + save flash |
| 0x10 | — | export fault log |
| 0x11 | — | write test fault |

---

## 2. STM32-Gateway-GUI — ครบ Feature

### Feature ที่เพิ่ม (เทียบ SonoPilotModbus)
- Repeat ComboBox, Mono toggle, Autoplay toggle, Goto track
- Signal Generator (type/freq/start/stop)
- RTC Sync (PC → device)
- Raw Register read/write
- FW version, Heap free, SD status, Format
- 32-bit Uptime (dd hh:mm:ss)
- MB baud + CAN baud dropdown + Set
- Poll Rate (100ms-5s) + Gap (10-200ms)
- Window position/size save/restore
- PC:921600 baud display

### 3 Tabs
1. **Log** — event log + Fault Log export + Test Fault
2. **Traffic** — TX (teal ซ้าย) / RX (yellow ขวา) live split view
3. **Register Map** — DataGrid ทุก register อัพเดทจาก poll live

### Fault Log Fix
- RxLoop กิน 0xFE response ก่อน BtnFaultLog อ่านได้
- แก้: RxLoop จับ 0xFE response → `_faultResponse` → BtnFaultLog รอ poll

---

## 3. SonoPilotModbus — แก้ Bug

- ลบ Bridge mode ออก (ใช้ transparent relay แทน)
- 32-bit uptime display จาก register 0x0020 + 0x0028
- Guard poll arrays ป้องกัน index out of bounds

---

## 4. ความรู้ / การตัดสินใจ

### STM32 Flash Internal
- Sector 3 (16KB) ใช้ EEPROM emulation ได้
- ต้อง erase ก่อน write ครั้งแรก (sector อาจไม่ blank)
- ต้อง `__disable_irq()` ระหว่าง flash write
- STM32_Programmer_CLI erase เฉพาะ sector ที่ binary ครอบ

### LittleFS vs EEPROM Emulation
- Internal flash erase = ทั้ง sector → LittleFS ต้องการ 2 sectors เท่ากัน
- Config ไม่กี่ค่า → EEPROM emulation เพียงพอ
- Fault log (เขียนตอน crash) → EEPROM emulation พอ
- Full syslog → ต้อง external SPI flash + LittleFS

### Bus Topology
- CAN/RS-485 Star → ต้อง hub (AND gate สำหรับ CAN / repeater สำหรับ RS-485)
- Daisy chain → ต่อตรง ไม่ต้อง hub
- DMX (1 ทาง) Star ได้ → splitter พอ
- Modbus/CAN (2 ทาง) Star → ต้อง hub ฉลาด
- MCP2515 SPI + 74HC138 decoder → CAN hub 8-32 port จาก MCU ตัวเดียว

### Redundant dual cable
- สาย 2 เส้นคนละทาง → สายขาด 1 เส้นยังคุมได้จากอีกฝั่ง
- Cat5e เส้นเดียว CAN+RS-485 คนละ pair → redundant ในสายเดียว

---

## Commits

### STM32-CAN-Bridge (feature/gateway)
- `de2f810` feat: dual mode RELAY/GATEWAY toggle
- `bd9c002` feat: boot default GATEWAY
- `eca8c47` fix: merge master (460800 + baud display)
- `739fa04` feat: PC baud 921600 + OLED CAN baud
- `7b40075` fix: 0xFE magic in gateway poll
- `e63e73a` feat: EEPROM emulation + fault log + CAN baud 20k-1M
- `ed3df07` fix: disable IRQ during flash write
- `0bd0c6d` fix: erase sector 3 on first boot
- `b5fbe8f` fix: remove debug flash test
- `a7c9bb1` fix: load saved MB baud on boot
- `6c6bf65` feat: test fault write 0xFE 0x11

### STM32-CAN-Bridge (master)
- `86b8846` feat: remote baud change 0xFE magic
- `a4d603c` feat: PC baud 460800

### STM32-Gateway-GUI
- `434381c` feat: initial — CAN + Modbus dual control
- `4711b9d` fix: parse Modbus response
- `22c1bda` feat: full poll + window save
- `13d1278` feat: 32-bit uptime
- `d958619` feat: full feature parity
- `8b36bee` fix: siggen delay
- `0fac656` feat: baud selector
- `769ffcd` fix: Set Baud bridge only
- `dab7302` feat: PC baud 921600
- `260445c` ui: show PC:921600
- `58822cb` feat: MB/CAN baud + Fault Log
- `86e1762` feat: Poll Rate + Gap
- `14b5007` fix: window width
- `a20dfb5` fix: ComboBox width
- `0f4af29` feat: Register Map tab
- `0d877b7` feat: Traffic Monitor tab
- `572255d` feat: Register Map live + TX/RX
- `3f86a28` fix: Register Map values in table
- `91cd22b` fix: Register Map from poll
- `b451cbd` feat: Test Fault button
- `a657454` fix: fault log via RxLoop

### SonoPilotModbus
- `47e1371` refactor: remove Bridge mode
- `b6a64aa` feat: 32-bit uptime

### rp2350-mp3-player
- `86e01e5` feat: 32-bit uptime register
- `6982d21` docs: session log
