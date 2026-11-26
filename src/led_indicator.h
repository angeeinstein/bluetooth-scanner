// RGB status-LED indicator (WS2812 boards only).
//
// Owns the LED and decides how the device's state is *shown* -- callers never
// deal in colours or patterns, they just report what happened (and the
// indicator reads most state itself from the usual getters). So re-hueing a
// state or changing a blink is a one-line change here, and nothing else moves.
//
// Plain-LED boards do NOT use this -- they keep their existing on/off patterns
// in power_ctrl.c. See STATUS_LED_WS2812 in pins.h. The colour palette and the
// state/animation choices live in led_indicator.c.
#pragma once

#include <stdbool.h>
#include <stdint.h>

void led_ind_init(void);

// Call once per poll tick (~20 Hz) from power_ctrl's poll_task.
//   hold_active: POWER button is being held (ship-hold in progress).
//   hold_on:     the already-computed blink phase for that hold (poll_task owns
//                the accelerating-blink timing, so the pattern matches the plain
//                LED exactly) -- shown as white while hold_active.
//   party:       request the rainbow (from power_ctrl_party_start()).
// Everything else -- recording, the SD/battery time-to-stop warning, errors,
// Wi-Fi (re)connects, self-test results, markers, start/stop -- the indicator
// reads/detects itself.
void led_ind_tick(bool hold_active, bool hold_on, bool party);

// Colour tester (temporary dev aid): hold a fixed colour while the LED is
// otherwise idle. It yields to -- and is cancelled by -- any real status, so it
// never fights an indication. active=false clears it.
void led_ind_set_test(bool active, uint8_t r, uint8_t g, uint8_t b);
