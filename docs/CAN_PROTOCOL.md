# SonoPilot CAN Bus Protocol

CAN 2.0B, 11-bit ID, max 8 bytes data.
Hardware: PIO2 via can2040, SN65HVD230 transceiver, GP26(TX)/GP27(RX).
Physical: Cat5e/Cat6 LAN cable, RJ45, daisy chain, 120Ω termination both ends.

## Message ID Map

| ID Range | Direction | Description |
|----------|-----------|-------------|
| `0x000` | → all | Broadcast (all nodes) |
| `0x010` | → group A | Multicast group A |
| `0x020` | → group B | Multicast group B |
| `0x030` | → group C | Multicast group C |
| `0x040` | → group D | Multicast group D |
| `0x100+N` | → node N | Command (unicast to node N) |
| `0x200+N` | ← node N | Track name (multi-frame, seq in data[0]) |
| `0x300+N` | ← node N | Status response (8 bytes) |
| `0x400+N` | ← node N | Extra info (reserved) |
| `0x500+H` | ← hub H | Sensor event (zone trigger) |

N = Player node ID (0x01–0x7F)
H = Sensor hub ID (0x01–0x0F)

## Communication Model

Two modes — configurable via OLED menu / EEPROM:

### 1. Heartbeat (push, default ON)

Node auto-sends 3 frames at configured rate (100ms–5s).
Can be disabled via OLED menu or EEPROM `can_hben=0`.

```
Node (auto)        Bus
    |--- Status (0x300+N) --->|
    |--- Name   (0x200+N) --->|
    |--- Extra  (0x400+N) --->|
    |     ... wait hb_rate ...|
    |--- Status (0x300+N) --->|  (repeat)
```

### 2. Request-response (GET)

Controller sends GET command → node responds with same 3 frames immediately.
Works regardless of heartbeat setting.

```
Controller           Node
    |--- GET (0x100+N) --->|
    |                      |--- Status (0x300+N) --->
    |                      |--- Name   (0x200+N) --->
    |                      |--- Extra  (0x400+N) --->
```

**Audio stutter note:** CAN TX burst (3 frames rapid) causes AHB bus contention with I2S audio DMA (PIO1).
If stutter occurs, disable heartbeat (`can_hben=0`) and use GET polling only.
GET response uses self-paced 1 frame per `GET_FRAME_DELAY_MS` (3ms) to minimize impact.

## 3-Frame Response

Used by both heartbeat and GET response. Same `send_3frame_response()` function:

### Frame 1: Status (`0x300 + N`, DLC=8)

| Byte | Field | Values |
|------|-------|--------|
| 0 | state | 0=stop, 1=play, 3=pause |
| 1 | track | track index (0-based) |
| 2 | volume | 0–100 |
| 3 | repeat | 0=All, 1=One, 2=Off, 3=Single, 4=Random |
| 4 | sample_khz | sampling rate ÷ 1000 (e.g. 44, 48, 96, 192) |
| 5 | elapsed_min | playback minutes |
| 6 | elapsed_sec | playback seconds |
| 7 | track_count | total tracks on SD |

### Frame 2: Name (`0x200 + N`, DLC=8)

| Byte | Field | Values |
|------|-------|--------|
| 0 | format | 0=unknown, 1=MP3, 2=WAV, 3=FLAC |
| 1–7 | name | track name (no extension), max 7 chars, null-padded |

### Frame 3: Extra (`0x400 + N`, DLC=8)

| Byte | Field | Values |
|------|-------|--------|
| 0 | temp | CPU temperature °C |
| 1 | group | 0=A, 1=B, 2=C, 3=D |
| 2 | multicast | 0=off, 1=on |
| 3 | mono | 0=stereo, 1=mono |
| 4 | autoplay | 0=off, 1=on |
| 5 | rtc_hour | RTC hour (0–23) |
| 6 | rtc_min | RTC minute (0–59) |
| 7 | rtc_sec | RTC second (0–59) |

### Extended Track Name (GET response only)

After 3-frame response, GET also sends full track name as multi-frame:

| ID | DLC | Byte 0 | Byte 1–7 |
|----|-----|--------|----------|
| `0x200+N` | 1–8 | seq (0,1,2...) | track name chars (ASCII) |

Full filename including extension, max 128 chars, 7 chars per frame.
seq=0 = first 7 chars, seq=1 = next 7, etc. Receiver clears buffer on seq=0.

## Commands (Controller → Node)

Sent to `0x100 + N` (unicast), `0x000` (broadcast), or `0x010–0x040` (group multicast).
**Fire-and-forget** — no response expected for commands. Use GET to confirm state.

### Simple Commands (DLC=2)

| data[0] | data[1] | Command |
|---------|---------|---------|
| `0x01` | 0 | Play |
| `0x02` | 0 | Stop |
| `0x03` | 0 | Next track |
| `0x04` | 0 | Previous track |
| `0x05` | 0 | Pause/Resume |
| `0x06` | 0–100 | Set volume |
| `0x07` | 0–4 | Set repeat (0=All, 1=One, 2=Off, 3=Single, 4=Random) |
| `0x08` | 0/1 | Set mono (0=stereo, 1=mono) |
| `0x09` | 0/1 | Set autoplay (0=off, 1=on) |
| `0x10` | 0 | GET — request status + track name response |

### Goto Track (DLC=2–8)

| Byte | Field |
|------|-------|
| 0 | `0x0C` |
| 1–7 | track name (ASCII, no extension, max 7 chars) |

Firmware matches name prefix against SD card files (case-insensitive, ignores extension).

### Signal Generator (DLC=2–5)

**Start** (DLC=5):

| Byte | Field |
|------|-------|
| 0 | `0x0A` |
| 1 | `0x00` (sub-cmd: start) |
| 2 | waveform type: 1=Sine, 2=Square, 3=Triangle, 4=Sawtooth, 5=White, 6=Pink |
| 3 | freq low byte |
| 4 | freq high byte |

Frequency: 1–20000 Hz, 16-bit little-endian. Must stop playback before starting siggen.

**Stop** (DLC=2):

| Byte | Field |
|------|-------|
| 0 | `0x0A` |
| 1 | `0x01` (sub-cmd: stop) |

**Change Frequency** (DLC=4):

| Byte | Field |
|------|-------|
| 0 | `0x0A` |
| 1 | `0x02` (sub-cmd: freq) |
| 2 | freq low byte |
| 3 | freq high byte |

### Set RTC Time (DLC=8)

| Byte | Field |
|------|-------|
| 0 | `0x0B` |
| 1 | year low byte |
| 2 | year high byte |
| 3 | month (1–12) |
| 4 | day (1–31) |
| 5 | hour (0–23) |
| 6 | minute (0–59) |
| 7 | second (0–59) |

Year: 16-bit little-endian (e.g. 2026 = `0xEA, 0x07`).
Sets both RP2350 AON RTC and DS3231 hardware RTC.

## Sensor Hub Events (Hub → Bus)

Sensor hub sends event frames when a sensor triggers. Player nodes receive and act on zone mapping.

### Sensor Event (`0x500 + H`, DLC=4–8)

| Byte | Field | Values |
|------|-------|--------|
| 0 | zone | 0–15 (sensor zone index) |
| 1 | trigger type | see table below |
| 2 | value | 0/1 (digital) or level (analog) |
| 3 | flags | bit0=rising, bit1=falling, bit2=sustained |
| 4–7 | reserved | 0 (future: threshold, duration) |

### Trigger Types

| Code | Type | Sensor | Use |
|------|------|--------|-----|
| `0x01` | PIR motion | HC-SR501, AM312 | คนเดินเข้าโซน |
| `0x02` | Light level | BH1750, LDR | แสงเปลี่ยน (เปิด/ปิดไฟ) |
| `0x03` | Beam break | IR pair | ผ่านจุดที่กำหนด |
| `0x04` | Door | Magnetic switch | ประตูเปิด/ปิด |
| `0x05` | Button | GPIO push | กดปุ่มหน้างาน |
| `0x06` | Ultrasonic | HC-SR04 | ระยะห่าง (value=cm) |
| `0x07` | Microwave | RCWL-0516 | motion ทะลุผนัง |
| `0x08` | Vibration | SW-420 | สั่นสะเทือน |
| `0x09` | Sound | KY-037 | เสียงดัง trigger |
| `0x0A` | Temperature | DS18B20, DHT22 | อุณหภูมิเกิน threshold |

### Player Zone Mapping (EEPROM config)

Each player node stores zone→action mapping:

```
zone_0_action = play        (0=ignore, 1=play, 2=stop, 3=next, 4=goto)
zone_0_track  = welcome     (track name prefix for goto)
zone_0_hub    = 0x01        (hub ID to listen, 0=any)
zone_1_action = stop
...
```

### Event Flow Example

```
[PIR zone 2 triggered]
  Hub 0x01 → CAN 0x501 [0x02, 0x01, 0x01, 0x01]
                         zone  PIR   HIGH  rising
  Player node 0x10 receives:
    zone_2_action = goto
    zone_2_track  = "ambient_zone_c"
    → sp_player_rc(GOTO, "ambient_zone_c")

[PIR zone 2 cleared]
  Hub 0x01 → CAN 0x501 [0x02, 0x01, 0x00, 0x02]
                         zone  PIR   LOW   falling
  Player node 0x10:
    zone_2_clear = stop (or fade, or ignore)
```

## USB-CAN Bridge Protocol (PicoCANBridge)

HID device (VID=0x1209, PID=0xCA01), 64-byte reports.

### PC → Bridge (HID Output Report)

| Byte | Field |
|------|-------|
| 0 | Report ID (0x00) |
| 1 | `0x01` (CMD_CAN_TX) |
| 2 | CAN ID high byte |
| 3 | CAN ID low byte |
| 4 | DLC (0–8) |
| 5–12 | CAN data (0–8 bytes) |

### Bridge → PC (HID Input Report)

| Byte | Field |
|------|-------|
| 0 | `0x02` (CMD_CAN_RX) |
| 1 | CAN ID high byte |
| 2 | CAN ID low byte |
| 3 | DLC (0–8) |
| 4–11 | CAN data (0–8 bytes) |

Bridge is transparent pass-through — all CAN RX frames forwarded to PC, all PC TX frames sent to CAN bus.

## Bus Topology

### Dual-bus on single Cat5e/Cat6 cable

```
RJ45 Pin Allocation:
  Pin 1,2 (pair 1) → CAN H / CAN L
  Pin 3,6 (pair 2) → RS-485 A / B
  Pin 4,5 (pair 3) → GND (shared)
  Pin 7,8 (pair 4) → spare / VCC (optional remote power)
```

CAN = primary control (real-time, event-driven)
Modbus RS-485 = backup control (poll-based, control room)

### Bus Limits

| Parameter | CAN | Modbus RS-485 |
|-----------|-----|---------------|
| Max nodes | 32 (CAN spec) | 32 (RS-485 spec) |
| Max length | 500m @ 125k | 1200m @ 9600 |
| Baud @ 200m | 125k–250k | 38,400–115,200 |
| Termination | 120Ω both ends | 120Ω both ends |
| Failure mode | Bus-off isolation | CRC drop, silent |

## Bus Error Behavior

- CAN bus error (cable disconnect, bus-off) → PIO2 killed immediately, no audio impact
- Auto-restart every 5 seconds when bus is down
- TX pin tri-stated during error (no interference)
- Audio DMA has bus priority (BUSCTRL_BUS_PRIORITY HIGH)

## Configuration (EEPROM)

| Key | Default | Description |
|-----|---------|-------------|
| `can_en` | `1` | CAN enable (0=PIO2 off, 1=on) |
| `can_id` | `0x10` | Node ID (0x01–0x7F) |
| `can_baud` | `0` | Baud index (0=125k, 1=250k, 2=500k, 3=1M) |
| `can_hben` | `1` | Auto heartbeat enable (0=off, 1=on) |
| `can_hbr` | `3` | Heartbeat rate index (0=100ms, 1=250ms, 2=500ms, 3=1s, 4=2s, 5=5s) |
| `can_grp` | `0` | Group (0=A, 1=B, 2=C, 3=D) |
| `can_mcast` | `0` | Multicast enable |

## CAN ↔ Modbus Command Mapping

| Feature | CAN Command | Modbus Register |
|---------|-------------|-----------------|
| Play | `0x100+N` data=[0x01, 0] | Write 0x0010 = 1 |
| Stop | `0x100+N` data=[0x02, 0] | Write 0x0010 = 2 |
| Next | `0x100+N` data=[0x03, 0] | Write 0x0010 = 3 |
| Prev | `0x100+N` data=[0x04, 0] | Write 0x0010 = 4 |
| Pause | `0x100+N` data=[0x05, 0] | Write 0x0010 = 5 |
| Volume | `0x100+N` data=[0x06, vol] | Write 0x0003 = vol |
| Repeat | `0x100+N` data=[0x07, mode] | Write 0x0004 = mode |
| Mono | `0x100+N` data=[0x08, 0/1] | Write 0x0005 = 0/1 |
| Autoplay | `0x100+N` data=[0x09, 0/1] | Write 0x0006 = 0/1 |
| Goto name | `0x100+N` data=[0x0C, name...] | *(not available)* |
| Siggen start | `0x100+N` data=[0x0A, 0, type, fL, fH] | Write 0x0013=type, 0x0014=freq, 0x0012=1 |
| Siggen stop | `0x100+N` data=[0x0A, 1] | Write 0x0012 = 0 |
| RTC set | `0x100+N` data=[0x0B, yL, yH, m, d, h, m, s] | Write 0x0015–0x001A |
| GET status | `0x100+N` data=[0x10, 0] | FC03 read 0x0000 count=9 |
| Sensor event | `0x500+H` data=[zone, type, val, flags] | *(CAN only)* |
