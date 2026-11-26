// Central GPIO pin map for the vibration-sensor board.
// Verified against schematic — see SELFTEST_SPEC.md. Do not change without
// re-checking the schematic; several of these are strapping-adjacent or
// PSRAM pins on other modules and must stay exactly as specified.

#pragma once

#include "driver/gpio.h"

// --- Accelerometer (IIS3DWB) — SPI, shared bus ---
#define PIN_ACCEL_CS    GPIO_NUM_4
#define PIN_ACCEL_MOSI  GPIO_NUM_5
#define PIN_ACCEL_SCLK  GPIO_NUM_6
#define PIN_ACCEL_MISO  GPIO_NUM_7
#define PIN_ACCEL_INT1  GPIO_NUM_15
// IO16 is a spare SPI line routed to the external connector; unused here.

// --- microSD — SDMMC 4-bit ---
#define PIN_SD_CLK      GPIO_NUM_14
#define PIN_SD_CMD      GPIO_NUM_13
#define PIN_SD_DAT0     GPIO_NUM_21
#define PIN_SD_DAT1     GPIO_NUM_47
#define PIN_SD_DAT2     GPIO_NUM_11
#define PIN_SD_DAT3     GPIO_NUM_12
#define PIN_SD_CD       GPIO_NUM_48

// --- Charger BQ25628 — I2C ---
#define PIN_CHG_SCL     GPIO_NUM_8
#define PIN_CHG_SDA     GPIO_NUM_9
#define PIN_CHG_INT     GPIO_NUM_10
#define PIN_CHG_CE      GPIO_NUM_18

// --- UI / misc ---
#define PIN_LED         GPIO_NUM_17   // status LED signal pin (see STATUS_LED_WS2812)

// Status-LED hardware select:
//   0 = original plain active-low LED on PIN_LED (the existing boards)
//   1 = WS2812C-2020 addressable RGB LED. Its DI sits on PIN_LED (through the
//       existing 100R series resistor), VDD on 3V3, DO left open. The pin is
//       then driven with the WS2812 protocol instead of a plain level.
// Leave this 0 for the plain-LED boards; set to 1 and rebuild for the RGB board.
#define STATUS_LED_WS2812 1

#define PIN_BTN_BOOT    GPIO_NUM_0    // pressed = LOW
#define PIN_BTN_POWER   GPIO_NUM_2    // pressed = LOW; NO internal pull (see notes)

// External sync trigger input (expansion header IO38). A shared rising edge fed
// to this pin on multiple boards latches each one's Wi-Fi TSF, for measuring /
// (later) enforcing cross-device time sync. Idle-low with an internal pulldown;
// drive it with a 0-3.3V logic signal ONLY -- never above 3.3V or negative.
#define PIN_SYNC_TRIG   GPIO_NUM_38

// USB D-/D+ on IO19/IO20 are the native USB console — never reconfigure.
// Expansion header: IO38, IO39, IO40, IO41, IO42, IO44 — free, unused here.

// Do NOT use: IO35/IO36/IO37 (octal PSRAM), IO19/IO20 (native USB),
// IO0/IO3/IO45/IO46 (strapping pins, IO0 excepted as BOOT button input).

#define LED_ON  0
#define LED_OFF 1
