# SonoPilot Modbus RTU Protocol

Modbus RTU Slave, UART0, 115200 8N1 (baud configurable).
Hardware: GP0(TX) / GP1(RX), GP9=RS-485 DE.
Supported FC: 03 (Read Holding), 06 (Write Single), 10 (Write Multiple).

## Register Map

### Status Registers (Read-Only, 0x0000–0x0008)

| Addr | Name | Description |
|------|------|-------------|
| 0x0000 | STATE | 0=stop, 1=play, 2=error, 3=pause |
| 0x0001 | TRACK | current track index (0-based) |
| 0x0002 | TRACK_COUNT | total tracks on SD |
| 0x0003 | VOLUME | 0–100 (RW) |
| 0x0004 | REPEAT | 0=All, 1=One, 2=Off, 3=Single, 4=Random (RW) |
| 0x0005 | MONO | 0=stereo, 1=mono (RW) |
| 0x0006 | AUTOPLAY | 0=off, 1=on (RW) |
| 0x0007 | SD_OK | 0=no SD, 1=SD mounted |
| 0x0008 | USB_OK | 1 (always) |

### Command Registers (Write-Only, 0x0010–0x001A)

| Addr | Name | Values |
|------|------|--------|
| 0x0010 | COMMAND | 1=play, 2=stop, 3=next, 4=prev, 5=pause |
| 0x0011 | GOTO_INDEX | track index (0-based), plays immediately |
| 0x0012 | SIGGEN_CMD | 0=stop, 1=start |
| 0x0013 | SIGGEN_TYPE | 1=Sine, 2=Square, 3=Triangle, 4=Sawtooth, 5=White, 6=Pink (RW) |
| 0x0014 | SIGGEN_FREQ | 1–20000 Hz (RW) |
| 0x0015 | RTC_YEAR | year e.g. 2026 (RW) |
| 0x0016 | RTC_MONTH | 1–12 (RW) |
| 0x0017 | RTC_DAY | 1–31 (RW) |
| 0x0018 | RTC_HOUR | 0–23 (RW) |
| 0x0019 | RTC_MIN | 0–59 (RW) |
| 0x001A | RTC_SEC | 0–59 (WO, write triggers `rtc_set_datetime`) |

**RTC Sync:** write Year→Month→Day→Hour→Min→Sec in order. Writing SEC triggers the actual RTC set.

### Info Registers (Read-Only, 0x0020–0x0027)

| Addr | Name | Description |
|------|------|-------------|
| 0x0020 | UPTIME | seconds since boot (wraps at 65535) |
| 0x0021 | TEMP_X10 | CPU temperature × 10 (e.g. 325 = 32.5°C) |
| 0x0022 | FW_MAJOR | firmware version major |
| 0x0023 | FW_MINOR | firmware version minor |
| 0x0024 | SLAVE_ADDR | current Modbus slave address |
| 0x0025 | HEAP_FREE | free heap ÷ 16 (bytes) |
| 0x0026 | SAMPLE_RATE | sample rate ÷ 100 (e.g. 441 = 44100 Hz) |
| 0x0027 | BAUDRATE | baud index (RW): 0=9600, 1=19200, 2=38400, 3=57600, 4=115200, 5=230400, 6=460800 |

### Snapshot Block (Read-Only, 0x0040–0x004B)

Full status in one read — same byte layout as CAN 3-frame heartbeat.

```
FC03  Start=0x0040  Count=12
```

Returns 24 bytes (12 registers, big-endian byte order per register):

| Reg | Hi byte | Lo byte | Source |
|-----|---------|---------|--------|
| 0x0040 | state | track | CAN Status |
| 0x0041 | volume | repeat | CAN Status |
| 0x0042 | sample_khz | elapsed_min | CAN Status |
| 0x0043 | elapsed_sec | track_count | CAN Status |
| 0x0044 | format | name[0] | CAN Name |
| 0x0045 | name[1] | name[2] | CAN Name |
| 0x0046 | name[3] | name[4] | CAN Name |
| 0x0047 | name[5] | name[6] | CAN Name |
| 0x0048 | temp | group | CAN Extra |
| 0x0049 | mcast | mono | CAN Extra |
| 0x004A | autoplay | rtc_hour | CAN Extra |
| 0x004B | rtc_min | rtc_sec | CAN Extra |

**Field values:**
- state: 0=stop, 1=play, 3=pause
- format: 0=unknown, 1=MP3, 2=WAV, 3=FLAC
- name: track name without extension, max 7 chars, null-padded
- sample_khz: sampling rate ÷ 1000 (e.g. 44, 48, 192)
- temp: CPU temperature °C
- group: 0=A, 1=B, 2=C, 3=D

### Track Name (Read-Only, 0x0100–0x010F)

Full filename, 32 chars max (16 registers, 2 chars per register, big-endian).

```
FC03  Start=0x0100  Count=16
```

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
| Goto index | — | Write 0x0011 = index |
| Goto name | `0x100+N` data=[0x0C, name...] | *(not available)* |
| Siggen start | `0x100+N` data=[0x0A, 0, type, fL, fH] | Write 0x0013=type, 0x0014=freq, 0x0012=1 |
| Siggen stop | `0x100+N` data=[0x0A, 1] | Write 0x0012 = 0 |
| Siggen freq | `0x100+N` data=[0x0A, 2, fL, fH] | Write 0x0014 = freq |
| RTC set | `0x100+N` data=[0x0B, yL, yH, m, d, h, m, s] | Write 0x0015–0x001A |
| GET status | `0x100+N` data=[0x10, 0] | FC03 read 0x0040 count=12 |

## Configuration (EEPROM)

| Key | Default | Description |
|-----|---------|-------------|
| `mb_addr` | `1` | Slave address (1–247) |
| `mb_baud` | `4` | Baud index (0=9600..6=460800) |
| `mb_grp` | `0` | Group (0=A, 1=B, 2=C, 3=D) |
| `mb_mcast` | `0` | Multicast enable |

## Physical Layer

- **RS-485** half-duplex, GP9 = DE/RE direction control
- **Broadcast** address 0x00 for sync start (all nodes respond)
- **Cable:** shared Cat5e/Cat6 with CAN bus (separate wire pair)
