// Minimal single-pixel WS2812 / WS2812C driver (RMT peripheral).
//
// Used only on boards whose status LED is an addressable RGB LED instead of a
// plain one -- see STATUS_LED_WS2812 in pins.h. The DI line sits on the same
// GPIO the plain LED used (PIN_LED), through the existing series resistor, so
// nothing else on the board changes.
//
// Colours are plain 8-bit R/G/B; the driver handles the GRB wire order and the
// WS2812 bit timing. Call from task context only (not an ISR).
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Sets up an RMT TX channel + WS2812 byte encoder on `gpio_num`. Returns false
// (and logs) on failure; safe to call more than once (later calls are no-ops).
bool led_rgb_init(int gpio_num);

// Latches one colour into the pixel. No-op if led_rgb_init() hasn't succeeded.
void led_rgb_set(uint8_t r, uint8_t g, uint8_t b);
