# Board Self-Test Firmware — Specification

## Goal

Write a **self-test firmware** for a custom ESP32-S3 vibration-sensor board. It must exercise every
subsystem and connection on the board and print a clear **PASS / FAIL report** to the USB serial
console. This is bring-up firmware for a freshly assembled prototype — the point is to find
assembly/connection faults, not to be production code.

**Framework:** ESP-IDF preferred (better SDMMC 4-bit + native USB console support). Arduino-ESP32 is
acceptable — pick one and stay consistent.

---

## Hardware Overview

| Item | Part |
|----|----|
| MCU module | **ESP32-S3-WROOM-1-N8R8** (8 MB flash, 8 MB octal PSRAM) |
| Console / flashing | **Native USB** (USB-Serial-JTAG) on the USB-C port — no UART bridge |
| Accelerometer | **ST IIS3DWB** (SPI, vibration sensor) |
| Charger / PMIC | **TI BQ25628** (I²C, 1-cell LiPo, NVDC power path) |
| Storage | microSD socket |
| Power chain | 12–60 V in → LMR51635 buck (5 V) → TPS2121 mux → BQ25628 → TPS63802 → 3V3 |

---

## Pinout (verified against schematic — use exactly these)

### Accelerometer — SPI (shared bus)
| Signal | GPIO |
|----|----|
| CS | **IO4** |
| MOSI (SDI) | **IO5** |
| SCLK | **IO6** |
| MISO (SDO) | **IO7** |
| INT1 | **IO15** |
| Spare (to ext. connector) | IO16 |

### microSD — SDMMC 4-bit (or SPI)
| Signal | GPIO |
|----|----|
| CLK | **IO14** |
| CMD | **IO13** |
| DAT0 | **IO21** |
| DAT1 | **IO47** |
| DAT2 | **IO11** |
| DAT3 / CS | **IO12** |
| Card Detect | **IO48** |

### Charger BQ25628 — I²C
| Signal | GPIO |
|----|----|
| SCL | **IO8** |
| SDA | **IO9** |
| INT | **IO10** |
| CE (charge enable) | **IO18** |

### UI / misc
| Signal | GPIO | Notes |
|----|----|----|
| User LED (blue) | **IO17** | ⚠️ **ACTIVE-LOW** (drive LOW = LED on) |
| BOOT button | IO0 | pressed = LOW |
| POWER button | **IO2** | pressed = LOW, ⚠️ see notes |
| USB D− / D+ | IO19 / IO20 | native USB — do not reconfigure |
| Expansion header | IO38, IO39, IO40, IO41, IO42, IO44 | free |

### Do NOT use
- **IO35, IO36, IO37** — octal PSRAM (N8R8). Using them will crash the board.
- **IO19 / IO20** — native USB.
- **IO0, IO3, IO45, IO46** — strapping pins.

---

## Tests to Implement

Run in this order, print `PASS` / `FAIL` per test plus a summary table at the end.

### 1. Boot & console
Print chip model, revision, CPU frequency, flash size, PSRAM size.
**PASS:** flash = 8 MB, PSRAM = 8 MB detected.

### 2. User LED
Blink **IO17** a few times. **Remember: ACTIVE-LOW** (LOW = on).
**PASS:** manual visual confirmation — prompt the user, wait for a keypress.

### 3. I²C bus scan
Scan the bus on **SCL=IO8, SDA=IO9** (100 kHz), print all addresses found.
**PASS:** device found at **0x6B** (BQ25628, 7-bit address).
*(If nothing is found: I²C wiring or charger power is the problem.)*

### 4. Charger communication
Read BQ25628 registers. Verify the part responds and read back status.
Use the built-in **12-bit ADC** to read **VBUS, VBAT, VSYS, charge current** and print them.
**PASS:** register reads return plausible values; VBUS ≈ 5 V when USB is connected.

*Expected reference values with USB connected and NO battery:*
- VBUS ≈ 5.0–5.2 V
- VSYS ≈ 3.75 V (VSYSMIN 3.52 V + 230 mV offset)

### 5. Accelerometer (IIS3DWB) — SPI
- SPI **mode 0 or 3**, clock **≤ 10 MHz** (hard limit for this sensor).
- Read **WHO_AM_I = register `0x0F`** → **expected value `0x7B`**.
- Then configure the sensor and read a few X/Y/Z samples; print raw + g values.
- Sanity check: with the board flat and still, Z should read ≈ ±1 g and X/Y ≈ 0 g.

**PASS:** WHO_AM_I == 0x7B **and** acceleration values are plausible (not all 0x0000 / 0xFFFF).

### 6. Accelerometer interrupt (optional)
Enable a data-ready interrupt, verify **IO15** toggles.
**PASS:** interrupt fires within a timeout.

### 7. microSD card
- Read **card detect (IO48)** and report whether a card is inserted.
- Mount the card (SDMMC 4-bit preferred, fall back to SPI).
- Print card type, size.
- **Write** a test file, **read it back**, **verify contents match**, then delete it.

**PASS:** mount OK, write/read/verify round-trip succeeds.

### 8. Buttons
Prompt the user to press **BOOT (IO0)** and then **POWER (IO2)**; detect each press with a timeout.
**PASS:** both presses detected (each reads LOW when pressed).

### 9. Wi-Fi
Initialize Wi-Fi in station mode and run an **AP scan**. Print the number of networks and the
strongest SSID/RSSI.
**PASS:** scan completes and finds ≥ 1 network (proves the radio + antenna work).

### 10. Bluetooth LE (optional)
Initialize BLE and start advertising for a few seconds.
**PASS:** init + advertising start return no error.

---

## Important Notes / Gotchas

- **User LED is ACTIVE-LOW** (3V3 → LED → resistor → IO17). Driving HIGH does nothing.
- **POWER button (IO2):** the button also drives the charger's QON pin; IO2 is connected through a
  100 kΩ series resistor. **Disable the ESP32 internal pull-up on IO2** — the external network already
  provides the high level, and the internal pull-up would form a divider and prevent reading a valid
  LOW. Configure as plain input, no pull.
- **IIS3DWB SPI clock must stay ≤ 10 MHz.** The bus is also routed to two external sensor connectors
  that share the same CS — only one device may be active at a time.
- **If the charger does not charge a connected battery:** the most likely cause is the TS
  (thermistor) pin. The board has solder pads for an external NTC; if no NTC is fitted, a **10 kΩ
  resistor must be soldered across the TS pads**, otherwise the charger reads "battery too cold" and
  suspends charging permanently. Report the charger's TS/fault status in test 4 so this is visible.
- Native USB is the console — make sure USB Serial/JTAG console output is enabled in menuconfig
  (ESP-IDF) or that `Serial` maps to USB CDC (Arduino).

---

## Output Format

End with a compact summary, for example:

```
==================== SELF TEST SUMMARY ====================
 1. Boot / chip info .................... PASS
 2. User LED ............................ PASS (visual)
 3. I2C scan ............................ PASS (0x6B found)
 4. Charger BQ25628 ..................... PASS (VBUS 5.11V, VSYS 3.74V)
 5. Accelerometer IIS3DWB ............... PASS (WHO_AM_I 0x7B)
 6. Accel interrupt ..................... PASS
 7. microSD ............................. PASS (7.4 GB, RW verified)
 8. Buttons ............................. PASS
 9. Wi-Fi scan .......................... PASS (12 APs)
10. BLE ................................. PASS
===========================================================
RESULT: 10/10 PASSED
```

Make each test independent and non-fatal — if one fails, log it and continue with the rest.
