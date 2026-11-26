#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool ok; // charger I2C reads succeeded
    int vbus_mv, vbat_mv, vsys_mv, ichg_ma;
    char ts_status[32];
    bool on_battery; // no USB/DC present and VBAT reads a plausible voltage
    bool battery_present; // a cell is actually connected (this board can run battery-less on USB)
    bool fault;
    bool ts_ignored; // TS pin monitoring disabled (no NTC populated)
    bool charging;   // CHG_STAT != "not charging/terminated" (only meaningful with USB present)
    char chg_phase[20];
    float tdie_c;                // charger die/junction temperature (degrees C)
    float mcu_temp_c;            // ESP32-S3 die temperature (degrees C); NAN if unavailable.
                                 // Read here (core 0, ~1 s) rather than by its consumers: the
                                 // IDF read briefly masks interrupts on the calling core.
    int ibus_ma;                 // input current drawn from USB
    bool thermal_regulation;     // charger reducing charge current due to die heat (TREG_STAT)
    bool input_current_limited;  // charge rate capped by the input current limit (IINDPM_STAT)
    bool input_voltage_limited;  // charge rate capped by input-voltage reg / source sag (VINDPM_STAT)

    // Coulomb-counted runtime estimate (see power_ctrl.c's update_battery_estimate()) --
    // a best-effort, self-improving estimate, not a precise reading.
    float batt_percent;         // 0-100, valid from boot (starts from the nominal capacity,
                                 // refines as learn_cycles grows)
    float batt_hours_remaining; // -1 if not applicable (charging, or no discharge-rate data yet)
    int batt_nominal_mah;       // user-set assumed capacity (see power_ctrl_reset_battery_learning())
    int batt_learned_mah;       // adaptively learned actual capacity
    int batt_learn_cycles;      // how many low-battery calibration points have contributed
    bool low_warning;           // on battery and at/below the low-voltage warning threshold
} power_status_t;

// Inits the charger (I2C, CE pin, ADC), the POWER button GPIO (no internal
// pull -- see pins.h), and starts the background poll task (button
// long-press detection + periodic charger status refresh). Call once at
// boot, after i2c_bus/recorder are available.
bool power_ctrl_start(void);

void power_ctrl_get_status(power_status_t *out);

// Enables/disables charger TS (thermistor) pin monitoring, persisting the
// choice to NVS so it survives reboots (re-applied at power_ctrl_start()).
// Pass true when no NTC is populated on the battery connector.
bool power_ctrl_set_ts_ignore(bool ignore);

// Manually triggers the same safe sequence as a POWER-button long press:
// stop/flush/close any active recording, then tell the charger to enter
// ship mode (BATFET_CTRL = SHIP). Blocks up to ~3s for the recording to
// close cleanly before issuing the I2C command.
void power_ctrl_trigger_ship_mode(void);

// Double-click POWER to start/stop a recording (with LED confirmation) is
// handled entirely inside power_ctrl.c's poll task -- no public API for
// that beyond this. Signals that boot/init is fully complete; call once,
// after the last init step in app_main (currently right after
// dashboard_start()). Fires a single quick LED flash on the next poll
// tick (<=50ms later), or the same "something's wrong" pattern used for a
// failed recording start if no SD card is mounted yet. Cross-task-safe
// (sets a flag consumed by the poll task, the same pattern recorder.c
// already uses for cross-task signaling via s_start_requested).
void power_ctrl_signal_ready(void);

// Resets the adaptive battery-runtime learning state -- call after
// physically changing the battery. Pass a positive mAh value to also set a
// new nominal capacity (persisted to NVS); pass <=0 to keep the existing
// nominal capacity and just clear the learned estimate/progress back to a
// fresh start. Deliberately not exposed anywhere in the dashboard UI --
// see the (also deliberately unlinked) GET /api/battery/reset in
// dashboard.c, the intended way to reach this.
void power_ctrl_reset_battery_learning(int nominal_mah);

// When enabled, a single short POWER-button click during a recording places a
// marker (timestamped at the press). Off by default; persisted to NVS.
// (Double-click still starts/stops; 3s hold still ships.)
void power_ctrl_set_button_mark(bool enable);
bool power_ctrl_get_button_mark(void);

// Kicks off a few seconds of colourful status-LED animation (a rainbow on the
// WS2812 RGB boards, a strobe on the plain ones) -- purely for fun. Non-blocking;
// the poll task drives it. Safe to call repeatedly (each call restarts the timer).
void power_ctrl_party_start(void);

// TEMPORARY dev aid: hold the RGB LED at a fixed colour (active=false clears it)
// so status colours can be eyeballed from the web UI. Remove with the palette set.
void power_ctrl_led_test(bool active, uint8_t r, uint8_t g, uint8_t b);
