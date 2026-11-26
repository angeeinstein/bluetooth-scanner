#include "power_ctrl.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bq25628.h"
#include "driver/gpio.h"
#include "driver/temperature_sensor.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "i2c_bus.h"
#include "led_indicator.h"
#include "led_rgb.h"
#include "nvs.h"
#include "pins.h"
#include "recorder.h"

#define NVS_NS "pwr_cfg"

// Fast-charge current target, applied once at boot. The BQ25628's own
// hardware ceiling is 2000mA (2A) -- for the current ~3000mAh test
// battery that's only ~0.67C, comfortably within standard safe LiPo
// charge rates even for a generic (non-high-rate) cell, so maxing it out
// here is a reasonable "how fast can this circuit actually go" test.
// If the battery changes: as a rule of thumb, keep this at or below the
// new cell's own 1C rating (numerically its mAh capacity) unless that
// cell is specifically rated for faster charging. Clamped to [40, 2000]
// by bq25628_set_charge_current_ma() regardless of what's set here.
#define CHARGE_CURRENT_MA 2000

// Adaptive battery-runtime estimate: coulomb-counts IBAT (already read every
// refresh_status() cycle as ichg_ma -- positive while charging, negative
// while discharging, per the BQ25628 datasheet) rather than trying to map
// VBAT to state-of-charge directly, since that curve is notoriously flat
// through the ~30-70% range -- no amount of learning fixes that, it's the
// cell chemistry, not a measurement problem. Two calibration anchors:
// "just finished a real charge" resets the running mAh-consumed counter to
// 0, and crossing a fixed, repeatable "getting low" voltage checkpoint
// treats the counter's value at that point as this cycle's capacity
// measurement, EMA'd into the learned estimate. Not literally 0%/100%,
// just two consistent, comparable reference points every cycle.
#define BATT_DEFAULT_CAPACITY_MAH 3000
// The low-battery WARNING trigger (on battery) -- see the low-battery
// protection block below. This is only a heads-up now, NOT the gauge's empty
// point: the fuel-gauge 0% anchor is BATT_GAUGE_EMPTY_MV (the hard cutoff), so
// the runtime estimate stays meaningful through the whole warning band down to
// the actual shutdown, instead of pinning to 0% the instant the warning fires.
#define BATT_LOW_THRESHOLD_MV 3600
#define BATT_FULL_VBAT_THRESHOLD_MV 4100 // must be at least this to count as "really" finished charging
// CHG_STAT (the charger's own "charging"/"terminated" status) is NOT used
// here -- it can sit in taper/CV mode indefinitely under concurrent system
// load, never reaching "terminated" even once the battery is effectively
// full (observed on real hardware: current visibly tapered to 0mA
// overnight while CHG_STAT still read "charging"). ichg_ma's raw current
// is the more direct, reliable signal. A small margin rather than a
// literal >0 comparison, since IBAT_ADC steps in 4mA increments and a
// residual/leakage current might hover near but never exactly at zero.
#define CHARGING_CURRENT_THRESHOLD_MA 20
#define BATT_LEARN_EMA_ALPHA 0.2f    // weight given to each new capacity measurement
// Smooths the instantaneous discharge rate for the hours-remaining estimate.
// The per-cycle current swings a lot (SD writes, Wi-Fi TX bursts), so this is
// a slow EMA -- ~10s time constant at the ~1s cycle -- enough that the shown
// estimate, and the low-battery warning that reads it, stay stable instead of
// jumping around with every processing spike.
#define BATT_RATE_EMA_ALPHA 0.1f
#define BATT_PERSIST_INTERVAL_CYCLES 60 // throttles the running-progress NVS write to ~once/minute

// Firmware low-battery protection. The BQ25628's own cutoff (VBAT_UVLO ~2.2V
// per the datasheet) is far too low for a LiPo, so we guard the cell
// ourselves. Two voltage thresholds (robust even if the fuel gauge is
// uncalibrated), each debounced against transient sag from WiFi TX / SD write
// current spikes:
//   WARN at BATT_LOW_THRESHOLD_MV, on battery: informational only. The status
//     bar shows "LOW" with the (smoothed) estimated time left and the RGB LED
//     pulses red. The device keeps running -- and recording -- so the user
//     gets the full usable runtime with a heads-up to wrap up or charge. There
//     is deliberately NO forced pre-emptive shutdown: the real 3.6->3.4V
//     runtime is long and hard to predict, and the HARD cutoff below already
//     protects the cell, so a fixed pre-emptive timer only threw away good
//     runtime.
//   HARD at BATT_HARD_MV: the single automatic power-off -- a non-overridable
//     graceful shutdown (stop/flush any recording first, then cut the BATFET)
//     to SHUTDOWN mode. Per the datasheet's BATFET control table, shutdown
//     mode -- unlike ship mode -- can ONLY be exited by plugging in USB, so a
//     nearly-empty cell can't be button-woken straight back into another
//     low-battery loop; coming back always means connecting a charger (which
//     also charges it).
#define BATT_HARD_MV 3400
// The fuel gauge's 0% / "empty" anchor: the voltage where usable capacity is
// considered spent, and where the gauge learns the cell's capacity (mAh drawn
// from full to here). Tied to the hard cutoff, since that's where the device
// actually powers off -- so 0% lines up with shutdown and both the dashboard
// runtime estimate and the low-battery warning's "time left" count the full
// usable range, including the 3.6->3.4V span below the warning threshold.
#define BATT_GAUGE_EMPTY_MV BATT_HARD_MV
#define BATT_LOW_CLEAR_MV 3700          // hysteresis: clear the LOW warning once VBAT recovers here
#define BATT_LOW_DEBOUNCE 6             // consecutive ~1s reads to confirm a threshold crossing

// Battery-presence detection. This board can run battery-less straight off USB.
// With no cell on the connector the charger's VBAT pin floats near 0 V (observed
// ~0.01-0.3 V, occasionally blipping as the charger probes for a battery),
// whereas any real Li-ion cell reads well above 2.5 V right down to the firmware
// hard cutoff (BATT_HARD_MV = 3.4 V). Debounced hysteresis on VBAT with a wide
// gap between the thresholds: declare PRESENT only after a sustained plausible
// reading and ABSENT only after a sustained implausibly-low one, so a charger
// probe-blip can never flip a battery-less board to "present", and a real cell
// (which never sags anywhere near 2 V before the device shuts itself off) is
// never misread as absent.
#define BATT_PRESENT_MV 2500
#define BATT_ABSENT_MV 2000
#define BATT_PRESENT_DEBOUNCE 5 // ~5 consecutive 1s reads to confirm a change
#define LOWBATT_BLINK_INTERVAL_MS 4000  // plain-LED boards: gap between low-battery warning blinks

#define LONG_PRESS_MS 3000
#define STATUS_REFRESH_TICKS 20 // 20 * 50ms = ~1s

// LED feedback while the POWER button is held: blink period shrinks from
// slow to fast as the hold approaches the long-press threshold, giving an
// accelerating visual countdown instead of the board looking unresponsive.
#define HOLD_BLINK_SLOW_MS 400
#define HOLD_BLINK_FAST_MS 60
#define SHIP_STOP_TIMEOUT_MS 3000
#define SHIP_BLINK_PERIOD_MS 100
// Hold the LED solid-on this long before actually commanding ship mode, so
// the "about to power off" confirmation is clearly visible. The charger's
// own turn-off is near-instant now (BATFET_DLY cleared in
// bq25628_set_batfet_ctrl()), so without this pause the solid-on phase
// would vanish too fast to read as a deliberate shutdown.
#define SHIP_CONFIRM_ON_MS 1500

// Double-click POWER: start/stop a recording without the web dashboard.
#define DOUBLE_CLICK_WINDOW_MS 500
// Matches SHIP_STOP_TIMEOUT_MS's existing budget for the same underlying
// recorder-close operation.
#define STOP_CONFIRM_TIMEOUT_MS 3000
// The writer task notices a start request on its next loop iteration
// (well under 100ms in practice) -- generous margin before treating it as
// a failed (async) open.
#define START_CONFIRM_TIMEOUT_MS 1000
#define HEARTBEAT_INTERVAL_MS 2000

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

// LED pattern vocabulary: 1/2/3 flashes forms an easy ready/started/saved
// mnemonic ladder; 7 rapid flashes is one consistent "something's wrong"
// signal shared by a failed start (rejected outright, or accepted but the
// file-open failed asynchronously), a stop that didn't confirm in time, a
// recording that stopped itself on a fatal write error (e.g. the card
// filled up), and a missing SD card at boot.
static const uint16_t PATTERN_BOOT_READY[] = {120};
static const uint16_t PATTERN_HEARTBEAT[] = {40, 40, 40}; // same speed as PATTERN_ERROR's segments
static const uint16_t PATTERN_STOP_SUCCESS[] = {70, 70, 70, 70, 70};
static const uint16_t PATTERN_ERROR[] = {40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40};
static const uint16_t PATTERN_STOP_PENDING[] = {150, 150}; // looped
static const uint16_t PATTERN_LOW_BATT[] = {50, 150, 50}; // two short blinks -- low-battery warning
static const uint16_t PATTERN_MARK[] = {90};              // one short flash -- marker placed

static bq25628_t s_chg;
static SemaphoreHandle_t s_status_mutex;
static power_status_t s_status;
static temperature_sensor_handle_t s_mcu_temp = NULL; // ESP32-S3 die sensor; NULL if unavailable
static bool s_ts_ignored = false;

static volatile bool s_ready_signal_pending = false;
static volatile bool s_ship_mode_active = false;

// Battery-runtime learning state -- see update_battery_estimate(). Plain
// statics rather than mutex-guarded, same lightweight precedent as
// s_ts_ignored above (power_ctrl_reset_battery_learning() writes from the
// HTTP handler task, update_battery_estimate() reads/writes from
// poll_task; worst case on a race is one ~1s-stale read, not a crash).
static int s_batt_nominal_mah = BATT_DEFAULT_CAPACITY_MAH;
static float s_batt_learned_mah = BATT_DEFAULT_CAPACITY_MAH;
static int s_batt_learn_cycles = 0;
static float s_batt_mah_consumed = 0.0f; // since the last detected full charge
static float s_batt_avg_discharge_ma = 0.0f;
static bool s_batt_was_charging = false;
static bool s_batt_was_above_low = true;
static int s_batt_persist_tick = 0;

// Low-battery protection state. Evaluated in refresh_status (under the status
// mutex); s_batt_hard_shutdown_pending crosses to poll_task, which performs the
// actual shutdown outside the mutex.
static bool s_batt_low_warn = false;              // display "LOW"
static int s_batt_soft_count = 0, s_batt_hard_count = 0; // crossing debounce
static volatile bool s_batt_hard_shutdown_pending = false;
static int64_t s_lowbatt_last_us = 0;

// Battery-presence state (see the BATT_PRESENT_* constants). Assume a cell is
// present until detection proves otherwise, so a normal board never flashes
// "no battery" at boot; a battery-less board settles to absent within a few
// seconds.
static bool s_battery_present = true;
static int s_batt_present_count = 0, s_batt_absent_count = 0;

static bool s_have_last_release = false;
static int64_t s_last_release_us = 0;
static int64_t s_last_press_us = 0;        // press time of a pending single-click (marker accuracy)
static bool s_button_mark_enabled = false; // single click during recording places a marker

static bool s_start_pending = false;
static int64_t s_start_deadline_us = 0;

static bool s_stop_pending = false;
static int64_t s_stop_deadline_us = 0;

static bool s_was_recording = false;
static int64_t s_heartbeat_last_us = 0;

// Non-blocking LED pattern player: an array of alternating ON/OFF
// millisecond segments (segment 0 is always ON), serviced once per
// poll_task tick. loop=false auto-turns the LED off after the last
// segment (one-shot confirmation patterns); loop=true wraps back to
// segment 0 indefinitely (the stop-pending "still working" blink).
static bool s_pattern_active = false;
static bool s_pattern_loop = false;
static const uint16_t *s_pattern_segments = NULL;
static uint8_t s_pattern_len = 0;
static uint8_t s_pattern_idx = 0;
static int64_t s_pattern_seg_start_us = 0;

#if STATUS_LED_WS2812
// On RGB boards the LED is driven entirely by led_indicator.c. The existing
// on/off pattern system still runs (its side effects matter), but its LED
// output here is inert -- so the two never fight over the pixel.
static inline void led_set(bool on) {
    (void)on;
}
#else
static inline void led_set(bool on) {
    gpio_set_level(PIN_LED, on ? 0 : 1); // active-low
}
#endif

// Ship/shutdown feedback. This runs in a blocking sequence (poll_task or the
// HTTP task is inside enter_low_power_mode, and the RGB indicator is held off),
// so it drives the pixel directly -- white on RGB boards, the plain LED otherwise.
static inline void ship_led(bool on) {
#if STATUS_LED_WS2812
    led_rgb_set(on ? 255 : 0, on ? 255 : 0, on ? 255 : 0);
#else
    led_set(on);
#endif
}

static void led_pattern_start(const uint16_t *segments, uint8_t len, bool loop) {
    s_pattern_segments = segments;
    s_pattern_len = len;
    s_pattern_loop = loop;
    s_pattern_idx = 0;
    s_pattern_seg_start_us = esp_timer_get_time();
    s_pattern_active = true;
    led_set(true); // segment 0 is always ON
}

static void led_pattern_service(void) {
    if (!s_pattern_active) {
        return;
    }
    int64_t now_us = esp_timer_get_time();
    if (now_us - s_pattern_seg_start_us < (int64_t)s_pattern_segments[s_pattern_idx] * 1000) {
        return;
    }
    s_pattern_idx++;
    if (s_pattern_idx >= s_pattern_len) {
        if (s_pattern_loop) {
            s_pattern_idx = 0;
        } else {
            led_set(false);
            s_pattern_active = false;
            return;
        }
    }
    led_set(s_pattern_idx % 2 == 0); // even index = ON segment
    s_pattern_seg_start_us = now_us;
}

// Called on POWER release when it's the second of two releases within
// DOUBLE_CLICK_WINDOW_MS. Toggles recording: reuses the exact same
// recorder_request_start()/_stop() calls the web dashboard uses, so
// behavior (and any concurrent-caller safety already relied on there) is
// identical regardless of which triggered it.
static void handle_double_click(void) {
    if (s_ship_mode_active) {
        return; // ship mode already owns shutdown + the LED
    }

    if (recorder_is_recording()) {
        recorder_request_stop();
        s_stop_pending = true;
        s_stop_deadline_us = esp_timer_get_time() + STOP_CONFIRM_TIMEOUT_MS * 1000;
        led_pattern_start(PATTERN_STOP_PENDING, ARRAY_LEN(PATTERN_STOP_PENDING), true);
    } else if (!recorder_request_start(NULL)) {
        led_pattern_start(PATTERN_ERROR, ARRAY_LEN(PATTERN_ERROR), false);
    } else {
        // Accepted, but the actual file-open is asynchronous (the writer
        // task does it on its next loop iteration) and can still fail
        // (e.g. SD present but full) -- service_start_pending() confirms
        // it actually started within a short timeout. No pattern fired
        // here on success: service_recording_heartbeat()'s edge-detect
        // notices recorder_is_recording() flip true and fires
        // PATTERN_HEARTBEAT itself, doubling as the start confirmation
        // (and covering dashboard-initiated starts the same way, for free).
        s_start_pending = true;
        s_start_deadline_us = esp_timer_get_time() + START_CONFIRM_TIMEOUT_MS * 1000;
    }
}

// A single short click (no second press within the double-click window). Only
// acts while recording, and only if the marker-on-click setting is enabled:
// places a marker timestamped at the press (s_last_press_us), so it reflects
// when the user pressed, not when the click resolved ~500ms later.
static void handle_single_click(void) {
    if (s_ship_mode_active) {
        return;
    }
    if (s_button_mark_enabled && recorder_is_recording()) {
        recorder_marker_add(s_last_press_us);
        led_pattern_start(PATTERN_MARK, ARRAY_LEN(PATTERN_MARK), false);
    }
}

// Polls (never blocks) while a button-initiated start is in flight, to
// catch the case where recorder_request_start() was accepted but the
// writer task's actual (asynchronous) file-open then failed -- e.g. the
// card is present but full. Success needs no action here: it just clears
// the flag and lets service_recording_heartbeat()'s edge-detect fire the
// confirmation, same as any other successful start.
static void service_start_pending(void) {
    if (!s_start_pending) {
        return;
    }
    if (recorder_is_recording()) {
        s_start_pending = false;
    } else if (esp_timer_get_time() >= s_start_deadline_us) {
        s_start_pending = false;
        led_pattern_start(PATTERN_ERROR, ARRAY_LEN(PATTERN_ERROR), false);
    }
}

// Polls (never blocks) while a button-initiated stop is in flight. Uses a
// plain state read rather than recorder_wait_for_stop()'s semaphore --
// power_ctrl_trigger_ship_mode() can be called from the dashboard's HTTP
// task concurrently with this running from poll_task, and two tasks
// consuming the same one-shot semaphore would race (whichever task's take
// wins gets true; the other times out and would wrongly report failure
// even though the recording saved fine). !recorder_is_recording() carries
// the identical information without that race -- recorder.c's writer task
// flips state to idle at exactly the point it would give the semaphore.
static void service_stop_pending(void) {
    if (!s_stop_pending) {
        return;
    }
    if (!recorder_is_recording()) {
        s_stop_pending = false;
        led_pattern_start(PATTERN_STOP_SUCCESS, ARRAY_LEN(PATTERN_STOP_SUCCESS), false);
    } else if (esp_timer_get_time() >= s_stop_deadline_us) {
        s_stop_pending = false;
        led_pattern_start(PATTERN_ERROR, ARRAY_LEN(PATTERN_ERROR), false);
    }
}

// Edge-detects a recording starting (any origin -- button or dashboard) to
// fire an immediate confirmation pulse, then repeats it periodically for
// as long as recording continues, so the LED gives an ongoing "yes, this
// is actively recording" signal even with no screen nearby. Also
// edge-detects a recording ending on its own (not via a tracked
// button-stop) with an error left behind -- e.g. the SD card filling up
// mid-recording -- so that's visible without a dashboard open too.
static void service_recording_heartbeat(void) {
    bool now_recording = recorder_is_recording();
    if (now_recording && !s_was_recording) {
        s_heartbeat_last_us = esp_timer_get_time();
        led_pattern_start(PATTERN_HEARTBEAT, ARRAY_LEN(PATTERN_HEARTBEAT), false);
    } else if (now_recording && !s_stop_pending &&
               esp_timer_get_time() - s_heartbeat_last_us >= HEARTBEAT_INTERVAL_MS * 1000) {
        s_heartbeat_last_us = esp_timer_get_time();
        led_pattern_start(PATTERN_HEARTBEAT, ARRAY_LEN(PATTERN_HEARTBEAT), false);
    } else if (!now_recording && s_was_recording && !s_stop_pending &&
               recorder_get_last_error()[0] != '\0') {
        led_pattern_start(PATTERN_ERROR, ARRAY_LEN(PATTERN_ERROR), false);
    }
    s_was_recording = now_recording;
}

static void service_boot_ready_signal(void) {
    if (!s_ready_signal_pending) {
        return;
    }
    s_ready_signal_pending = false;
    if (recorder_is_sd_mounted()) {
        led_pattern_start(PATTERN_BOOT_READY, ARRAY_LEN(PATTERN_BOOT_READY), false);
    } else {
        // Same "something's wrong" vocabulary as a failed button-start --
        // visible without needing to open the dashboard.
        led_pattern_start(PATTERN_ERROR, ARRAY_LEN(PATTERN_ERROR), false);
    }
}

void power_ctrl_signal_ready(void) {
    s_ready_signal_pending = true;
}

// Periodic low-battery LED blink while idle -- a heads-up when there's no
// screen. Suppressed during recording (the recording heartbeat owns the LED
// then) and whenever another pattern is mid-play, so it never fights.
static void service_low_battery_warning(void) {
    if (!s_batt_low_warn || recorder_is_recording() || s_pattern_active) {
        return;
    }
    if (esp_timer_get_time() - s_lowbatt_last_us >= (int64_t)LOWBATT_BLINK_INTERVAL_MS * 1000) {
        s_lowbatt_last_us = esp_timer_get_time();
        led_pattern_start(PATTERN_LOW_BATT, ARRAY_LEN(PATTERN_LOW_BATT), false);
    }
}

// Graceful low-power shutdown: stop/flush any recording, then cut the BATFET.
// `batfet_mode` is BQ25628_BATFET_SHIP (button- or USB-wakeable) or
// BQ25628_BATFET_SHUTDOWN (USB-only wake -- used for the hard low-battery
// cutoff so a near-empty cell can't be button-looped back on).
static void enter_low_power_mode(bq25628_batfet_ctrl_t batfet_mode) {
    // Idempotent guard: this can be reached from multiple tasks -- poll_task's
    // long-press branch, its low-battery handling, and
    // power_ctrl_trigger_ship_mode() (the dashboard's HTTP task) -- and two
    // entrants would race on recorder_wait_for_stop()'s one-shot semaphore.
    // Bailing on the second entrant keeps a single owner of the shutdown/LED
    // sequence. (The failure branch below clears the flag, so a failed attempt
    // can still be retried.)
    if (s_ship_mode_active) {
        return;
    }
    s_ship_mode_active = true;
    const char *mode_name = (batfet_mode == BQ25628_BATFET_SHUTDOWN) ? "shutdown" : "ship";
    printf("power_ctrl: entering %s mode -- stopping any active recording first\n", mode_name);
    recorder_request_stop();

    // Blink rapidly while waiting for the recorder to flush/close (if a
    // recording was active), so ship-mode entry gives visible feedback
    // instead of looking frozen during the up-to-3s stop sequence.
    int64_t deadline_us = esp_timer_get_time() + SHIP_STOP_TIMEOUT_MS * 1000;
    bool led_on = false;
    while (!recorder_wait_for_stop(0)) {
        if (esp_timer_get_time() >= deadline_us) {
            break;
        }
        led_on = !led_on;
        ship_led(led_on);
        vTaskDelay(pdMS_TO_TICKS(SHIP_BLINK_PERIOD_MS));
    }

    ship_led(true); // solid on: about to write BATFET_CTRL and cut power
    vTaskDelay(pdMS_TO_TICKS(SHIP_CONFIRM_ON_MS)); // brief, clearly-visible confirmation first
    if (!bq25628_set_batfet_ctrl(&s_chg, batfet_mode)) {
        printf("power_ctrl: failed to write BATFET_CTRL=%s\n", mode_name);
        ship_led(false); // didn't actually power off -- don't leave it stuck on
        s_ship_mode_active = false;
    }
    // else: leave solid on and s_ship_mode_active set -- board is about to lose power anyway.
}

static void persist_battery_progress(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, "batt_used_x10", (int32_t)(s_batt_mah_consumed * 10.0f));
        nvs_commit(h);
        nvs_close(h);
    }
}

static void persist_battery_learning(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, "batt_lrn_mah", (int32_t)(s_batt_learned_mah + 0.5f));
        nvs_set_i32(h, "batt_lrn_n", s_batt_learn_cycles);
        nvs_commit(h);
        nvs_close(h);
    }
}

// Coulomb-counts ichg_ma (positive while charging, negative while
// discharging) into s_batt_mah_consumed and updates the adaptively learned
// capacity at the two calibration anchors described where the BATT_*
// constants are defined. Called once per refresh_status() cycle (~1s),
// only when this cycle's I2C reads succeeded. Caller must hold
// s_status_mutex -- writes s_status.batt_* directly, same as the rest of
// refresh_status().
static void update_battery_estimate(int vbat_mv, int ichg_ma, bool on_battery) {
    if (!s_battery_present) {
        // No cell -> the coulomb-count integrator is meaningless. Report "no
        // reading" (percent < 0) so the dashboard shows no battery gauge rather
        // than a phantom 100%.
        s_status.batt_percent = -1.0f;
        s_status.batt_hours_remaining = -1.0f;
        s_status.batt_nominal_mah = s_batt_nominal_mah;
        s_status.batt_learned_mah = (int)(s_batt_learned_mah + 0.5f);
        s_status.batt_learn_cycles = s_batt_learn_cycles;
        return;
    }
    bool charging_now = ichg_ma > CHARGING_CURRENT_THRESHOLD_MA;

    if (s_batt_was_charging && !charging_now && !on_battery && vbat_mv > BATT_FULL_VBAT_THRESHOLD_MV) {
        // Just finished a real charge (not merely "not charging" because of
        // a fault/disable while still low) -- snap back to exactly 0
        // regardless of any accumulated integration drift.
        s_batt_mah_consumed = 0.0f;
    }
    s_batt_was_charging = charging_now;

    const float dt_hours = 1.0f / 3600.0f; // ~1s between refresh_status() calls

    if (charging_now) {
        // Live-tracks upward while charging too (not just snapping to 100%
        // at completion) -- charging efficiency isn't perfect, so this
        // drifts a little, but it's corrected back to exactly 0 at the
        // completion anchor above regardless.
        s_batt_mah_consumed -= (float)ichg_ma * dt_hours;
        if (s_batt_mah_consumed < 0.0f) {
            s_batt_mah_consumed = 0.0f;
        }
        s_batt_was_above_low = true; // rearm for the next real discharge cycle
    } else if (on_battery && ichg_ma < 0) {
        float discharge_ma = (float)(-ichg_ma);
        s_batt_mah_consumed += discharge_ma * dt_hours;
        s_batt_avg_discharge_ma = (s_batt_avg_discharge_ma <= 0.0f)
                                       ? discharge_ma
                                       : (BATT_RATE_EMA_ALPHA * discharge_ma +
                                          (1.0f - BATT_RATE_EMA_ALPHA) * s_batt_avg_discharge_ma);

        if (s_batt_was_above_low && vbat_mv <= BATT_GAUGE_EMPTY_MV) {
            s_batt_learned_mah = (s_batt_learn_cycles == 0)
                                      ? s_batt_mah_consumed
                                      : (BATT_LEARN_EMA_ALPHA * s_batt_mah_consumed +
                                         (1.0f - BATT_LEARN_EMA_ALPHA) * s_batt_learned_mah);
            s_batt_learn_cycles++;
            persist_battery_learning();
        }
        s_batt_was_above_low = vbat_mv > BATT_GAUGE_EMPTY_MV;
    }

    if (++s_batt_persist_tick >= BATT_PERSIST_INTERVAL_CYCLES) {
        s_batt_persist_tick = 0;
        persist_battery_progress();
    }

    float remaining_mah = s_batt_learned_mah - s_batt_mah_consumed;
    if (remaining_mah < 0.0f) {
        remaining_mah = 0.0f;
    } else if (remaining_mah > s_batt_learned_mah) {
        remaining_mah = s_batt_learned_mah;
    }
    s_status.batt_percent =
        (s_batt_learned_mah > 0.0f) ? (remaining_mah / s_batt_learned_mah * 100.0f) : 0.0f;
    s_status.batt_hours_remaining = (on_battery && !charging_now && s_batt_avg_discharge_ma > 0.0f)
                                         ? (remaining_mah / s_batt_avg_discharge_ma)
                                         : -1.0f;
    s_status.batt_nominal_mah = s_batt_nominal_mah;
    s_status.batt_learned_mah = (int)(s_batt_learned_mah + 0.5f);
    s_status.batt_learn_cycles = s_batt_learn_cycles;
}

// Updates the debounced battery-presence state from this cycle's VBAT reading.
// Caller holds s_status_mutex. Between the two thresholds the current state is
// held (hysteresis), so a reading drifting through the band never flips it.
static void detect_battery_present(int vbat_mv) {
    if (vbat_mv >= BATT_PRESENT_MV) {
        s_batt_absent_count = 0;
        if (s_batt_present_count < BATT_PRESENT_DEBOUNCE) s_batt_present_count++;
        if (s_batt_present_count >= BATT_PRESENT_DEBOUNCE) s_battery_present = true;
    } else if (vbat_mv <= BATT_ABSENT_MV) {
        s_batt_present_count = 0;
        if (s_batt_absent_count < BATT_PRESENT_DEBOUNCE) s_batt_absent_count++;
        if (s_batt_absent_count >= BATT_PRESENT_DEBOUNCE) s_battery_present = false;
    }
    s_status.battery_present = s_battery_present;
}

// Low-battery protection (see the BATT_* constants). Called each ~1s status
// refresh with s_status_mutex held: sets the warning flag + s_status.low_warning
// and, at the hard cutoff, arms s_batt_hard_shutdown_pending (poll_task performs
// the actual shutdown outside the mutex). No pre-emptive soft shutdown: the
// device runs to the hard cutoff, which is the only automatic power-off.
static void evaluate_low_battery(int vbat_mv) {
    if (!s_status.on_battery) {
        // Plugged in -- nothing to protect against; clear everything.
        s_batt_low_warn = false;
        s_batt_soft_count = s_batt_hard_count = 0;
        s_status.low_warning = false;
        return;
    }

    // HARD cutoff -- the single automatic power-off. A critically low cell must
    // bail regardless; debounced so a transient sag doesn't trip it.
    if (vbat_mv <= BATT_HARD_MV) {
        if (++s_batt_hard_count >= BATT_LOW_DEBOUNCE) {
            s_batt_hard_shutdown_pending = true;
        }
    } else {
        s_batt_hard_count = 0;
    }

    // LOW warning -- informational only (status bar + LED). Hysteresis: it
    // latches on at the threshold and clears only once the cell has recovered
    // well past it, so it doesn't flicker on/off around 3.6V.
    if (vbat_mv <= BATT_LOW_THRESHOLD_MV) {
        if (s_batt_soft_count < BATT_LOW_DEBOUNCE) {
            s_batt_soft_count++;
        }
        if (s_batt_soft_count >= BATT_LOW_DEBOUNCE) {
            s_batt_low_warn = true;
        }
    } else if (vbat_mv >= BATT_LOW_CLEAR_MV) {
        s_batt_low_warn = false;
        s_batt_soft_count = 0;
    }

    s_status.low_warning = s_batt_low_warn;
}

static void refresh_status(void) {
    // Pets the charger's I2C watchdog (POR default 50s timeout -- see
    // BQ25628_CTRL0_WD_RST's comment). refresh_status() already runs on a
    // ~1s cadence, comfortably inside that window; without this, the
    // charger silently reverts TS_IGNORE (and disables the ADC) once the
    // watchdog expires, regardless of what was configured at boot.
    bq25628_reset_watchdog(&s_chg);

    uint8_t status0, status1, fault0, vbus_stat;
    int vbus_mv = 0, vbat_mv = 0, vsys_mv = 0, ichg_ma = 0, ibus_ma = 0;
    float tdie_c = 0;
    bool ok = bq25628_read_status(&s_chg, &status0, &status1, &fault0) &&
              bq25628_read_vbus_stat(&s_chg, &vbus_stat) &&
              bq25628_read_vbus_mv(&s_chg, &vbus_mv) && bq25628_read_vbat_mv(&s_chg, &vbat_mv) &&
              bq25628_read_vsys_mv(&s_chg, &vsys_mv) && bq25628_read_ibat_ma(&s_chg, &ichg_ma) &&
              bq25628_read_ibus_ma(&s_chg, &ibus_ma) && bq25628_read_tdie_c(&s_chg, &tdie_c);

    // ESP32-S3 die temperature. Read on this core-0 task (not by the recorder on
    // core 1): IDF takes it inside a critical section -- a few us, ~300 us when the
    // sensor switches range -- and the Wi-Fi PHY already reads it the same way here.
    float mcu_c = NAN;
    if (s_mcu_temp != NULL && temperature_sensor_get_celsius(s_mcu_temp, &mcu_c) != ESP_OK) {
        mcu_c = NAN;
    }

    if (xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }
    s_status.mcu_temp_c = mcu_c; // independent of the charger read below
    s_status.ok = ok;
    if (ok) {
        s_status.vbus_mv = vbus_mv;
        s_status.vbat_mv = vbat_mv;
        s_status.vsys_mv = vsys_mv;
        s_status.ichg_ma = ichg_ma;
        strncpy(s_status.ts_status, bq25628_ts_status_str(fault0), sizeof(s_status.ts_status) - 1);
        s_status.ts_status[sizeof(s_status.ts_status) - 1] = '\0';
        strncpy(s_status.chg_phase, bq25628_chg_stat_str(status1), sizeof(s_status.chg_phase) - 1);
        s_status.chg_phase[sizeof(s_status.chg_phase) - 1] = '\0';
        s_status.tdie_c = tdie_c;
        s_status.ibus_ma = ibus_ma;
        s_status.thermal_regulation = (status0 & BQ25628_STATUS0_TREG_STAT) != 0;
        s_status.input_current_limited = (status0 & BQ25628_STATUS0_IINDPM_STAT) != 0;
        s_status.input_voltage_limited = (status0 & BQ25628_STATUS0_VINDPM_STAT) != 0;

        detect_battery_present(vbat_mv);
        bool present = s_battery_present;
        // With no cell connected the board runs from USB; force the
        // battery-derived signals off so nothing shows a phantom cell, reports
        // "charging"/"on battery", or raises a battery fault. (Presence gates
        // low_warning too, indirectly: on_battery is false without a cell, and
        // evaluate_low_battery only warns while on_battery.)
        s_status.charging = present && (status1 & BQ25628_STATUS1_CHG_STAT_MASK) != 0;
        // No USB/DC input detected, and VBAT reads a plausible battery
        // voltage (not floating near 0V) -- running on battery.
        s_status.on_battery =
            present && (vbus_stat == BQ25628_VBUS_STAT_NOT_POWERED) && (vbat_mv > 2500);
        s_status.fault = (fault0 & (BQ25628_FAULT_VBUS_FAULT_STAT | BQ25628_FAULT_SYS_FAULT_STAT |
                                     BQ25628_FAULT_TSHUT_STAT)) != 0 ||
                         (present && (fault0 & BQ25628_FAULT_BAT_FAULT_STAT) != 0);
        update_battery_estimate(vbat_mv, ichg_ma, s_status.on_battery);
        evaluate_low_battery(vbat_mv);
    }
    s_status.ts_ignored = s_ts_ignored;
    xSemaphoreGive(s_status_mutex);
}

// --- Party mode: a few seconds of colourful LED, just for fun ---------------
#define PARTY_DURATION_MS 6000
static int64_t s_party_until_us = 0;

void power_ctrl_party_start(void) {
    s_party_until_us = esp_timer_get_time() + (int64_t)PARTY_DURATION_MS * 1000;
}
static inline bool party_active(void) {
    return esp_timer_get_time() < s_party_until_us;
}

#if !STATUS_LED_WS2812
// Plain-LED party: a fast strobe. (On RGB boards the rainbow lives in
// led_indicator.c, driven by the party flag passed to led_ind_tick().)
static bool s_party_running = false;
static void service_party(void) {
    int64_t now = esp_timer_get_time();
    if (now < s_party_until_us) {
        s_party_running = true;
        led_set(((now / 90000) % 2) == 0);
    } else if (s_party_running) {
        s_party_running = false;
        led_set(false);
    }
}
#endif

// TEMPORARY colour tester -- forwards to the RGB indicator (an RGB-only aid).
void power_ctrl_led_test(bool active, uint8_t r, uint8_t g, uint8_t b) {
#if STATUS_LED_WS2812
    led_ind_set_test(active, r, g, b);
#else
    (void)active;
    (void)r;
    (void)g;
    (void)b;
#endif
}

static void poll_task(void *arg) {
    int64_t press_start_us = 0;
    bool press_active = false;
    bool long_press_fired = false;
    int status_tick = STATUS_REFRESH_TICKS; // refresh immediately on first loop
    bool hold_led_on = false;
    int64_t hold_led_last_toggle_us = 0;

    for (;;) {
        int hold_progress = -1; // 0..1000 while the POWER button is held (for led_indicator)
        bool pressed = gpio_get_level(PIN_BTN_POWER) == 0;
        if (pressed) {
            if (!press_active) {
                press_active = true;
                press_start_us = esp_timer_get_time();
                hold_led_last_toggle_us = press_start_us;
                hold_led_on = false;
                long_press_fired = false;
            } else if (!long_press_fired) {
                int64_t now_us = esp_timer_get_time();
                int64_t held_ms = (now_us - press_start_us) / 1000;
                if (held_ms >= LONG_PRESS_MS) {
                    long_press_fired = true;
                    // Ship mode takes full ownership of the stop-confirm
                    // sequence and the LED from here -- it already calls
                    // recorder_request_stop() itself (no-op-safe if a
                    // button-initiated stop was already requested).
                    s_stop_pending = false;
                    s_start_pending = false;
                    enter_low_power_mode(BQ25628_BATFET_SHIP);
                } else {
                    float progress = (float)held_ms / LONG_PRESS_MS;
                    hold_progress = (int)(progress * 1000.0f); // -> led_indicator white ramp
                    int half_period_ms = (int)(HOLD_BLINK_SLOW_MS -
                                                (HOLD_BLINK_SLOW_MS - HOLD_BLINK_FAST_MS) * progress);
                    if (now_us - hold_led_last_toggle_us >= (int64_t)half_period_ms * 1000) {
                        hold_led_on = !hold_led_on;
                        led_set(hold_led_on);
                        hold_led_last_toggle_us = now_us;
                    }
                }
            }
        } else {
            if (press_active && !long_press_fired) {
                led_set(false); // released before the long-press threshold -- stop blinking

                int64_t now_us = esp_timer_get_time();
                if (s_have_last_release &&
                    now_us - s_last_release_us <= DOUBLE_CLICK_WINDOW_MS * 1000) {
                    s_have_last_release = false; // consume the pair so a triple-click
                                                  // doesn't fire twice
                    handle_double_click();
                } else {
                    s_have_last_release = true;
                    s_last_release_us = now_us;
                    s_last_press_us = press_start_us; // remember when this click was pressed
                }
            }
            press_active = false;
        }

        // A held button (still deciding hold-vs-click) keeps exclusive
        // ownership of the LED via the branch above; only service the new
        // patterns once it's released, and never while ship mode owns the
        // shutdown sequence.
        if (!press_active && !s_ship_mode_active) {
            // A pending single click resolves once the double-click window has
            // passed with no second press -> place a marker (if enabled).
            if (s_have_last_release &&
                esp_timer_get_time() - s_last_release_us > DOUBLE_CLICK_WINDOW_MS * 1000) {
                s_have_last_release = false;
                handle_single_click();
            }
            service_boot_ready_signal();
            service_stop_pending();
            service_start_pending();
            service_recording_heartbeat();
            service_low_battery_warning();
            led_pattern_service();
#if !STATUS_LED_WS2812
            service_party(); // plain-LED party strobe (RGB rainbow is in led_indicator)
#endif
        }

        // Low-battery hard cutoff (armed by evaluate_low_battery). Done here,
        // not in refresh_status, so it runs outside the status mutex. Goes to
        // SHUTDOWN mode (USB-only wake) so a nearly-empty cell can't be
        // button-looped straight back into another low-battery cycle.
        if (s_batt_hard_shutdown_pending && !s_ship_mode_active) {
            s_batt_hard_shutdown_pending = false;
            printf("power_ctrl: battery critically low (<= %dmV) -- shutdown mode (plug in USB)\n",
                   BATT_HARD_MV);
            s_stop_pending = false;
            s_start_pending = false;
            enter_low_power_mode(BQ25628_BATFET_SHUTDOWN);
        }

        if (++status_tick >= STATUS_REFRESH_TICKS) {
            status_tick = 0;
            refresh_status();
        }

#if STATUS_LED_WS2812
        // On RGB boards the indicator is the sole LED driver: it reads device
        // state itself and renders colour + animation (the on/off calls above
        // are inert here). hold_progress and the party flag are the only bits
        // it can't read on its own. Held off during ship mode, whose blocking
        // sequence drives the pixel itself (ship_led).
        if (!s_ship_mode_active) {
            led_ind_tick(hold_progress >= 0, hold_led_on, party_active());
        }
#endif

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

bool power_ctrl_start(void) {
    gpio_config_t ce_conf = {
        .pin_bit_mask = 1ULL << PIN_CHG_CE,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&ce_conf);
    // CE is active-low; drive low so the charger actively regulates.
    gpio_set_level(PIN_CHG_CE, 0);

    // POWER (IO2): external 100k network already provides the high level;
    // the internal pull-up must stay OFF or it forms a divider that
    // prevents a valid LOW read.
    gpio_config_t power_btn_conf = {
        .pin_bit_mask = 1ULL << PIN_BTN_POWER,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&power_btn_conf);

#if STATUS_LED_WS2812
    // Addressable RGB status LED: drive PIN_LED with the WS2812 protocol.
    if (!led_rgb_init(PIN_LED)) {
        printf("power_ctrl: WS2812 status LED init failed -- continuing\n");
    }
    // Boot self-test so the RGB wiring can be verified at a glance: red, green,
    // blue, then off. Harmless on the plain-LED boards because it's compiled out.
    led_rgb_set(60, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(200));
    led_rgb_set(0, 60, 0);
    vTaskDelay(pdMS_TO_TICKS(200));
    led_rgb_set(0, 0, 60);
    vTaskDelay(pdMS_TO_TICKS(200));
    led_rgb_set(0, 0, 0); // off at boot
    led_ind_init();       // hand the LED over to the status indicator
#else
    gpio_config_t led_conf = {
        .pin_bit_mask = 1ULL << PIN_LED,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&led_conf);
    led_set(false); // off at boot
#endif

    i2c_master_bus_handle_t bus = i2c_bus_get();
    if (bus == NULL) {
        return false;
    }
    if (!bq25628_init(bus, &s_chg)) {
        return false;
    }
    bq25628_adc_enable(&s_chg);

    uint8_t saved_ts_ignore = 0;
    nvs_handle_t nvs_h;
    if (nvs_open(NVS_NS, NVS_READONLY, &nvs_h) == ESP_OK) {
        nvs_get_u8(nvs_h, "ts_ignore", &saved_ts_ignore);
        uint8_t btn_mark = 0;
        nvs_get_u8(nvs_h, "btn_mark", &btn_mark);
        s_button_mark_enabled = btn_mark != 0;
        int32_t v;
        if (nvs_get_i32(nvs_h, "batt_nom_mah", &v) == ESP_OK) {
            s_batt_nominal_mah = v;
        }
        if (nvs_get_i32(nvs_h, "batt_lrn_mah", &v) == ESP_OK) {
            s_batt_learned_mah = (float)v;
        }
        if (nvs_get_i32(nvs_h, "batt_lrn_n", &v) == ESP_OK) {
            s_batt_learn_cycles = v;
        }
        if (nvs_get_i32(nvs_h, "batt_used_x10", &v) == ESP_OK) {
            s_batt_mah_consumed = (float)v / 10.0f;
        }
        nvs_close(nvs_h);
    }
    s_ts_ignored = saved_ts_ignore != 0;
    if (!bq25628_set_ts_ignore(&s_chg, s_ts_ignored)) {
        printf("power_ctrl: failed to apply saved TS-ignore setting\n");
    }
    if (s_batt_learned_mah <= 0.0f) {
        s_batt_learned_mah = (float)s_batt_nominal_mah; // sane fallback, e.g. first-ever boot
    }

    if (!bq25628_set_charge_current_ma(&s_chg, CHARGE_CURRENT_MA)) {
        printf("power_ctrl: failed to set charge current\n");
    }

    // ESP32-S3 internal temperature sensor (status.mcu_temp_c). Optional: on any
    // failure the value just stays NAN -- never a reason to fail start-up.
    s_status.mcu_temp_c = NAN;
    temperature_sensor_config_t tcfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    if (temperature_sensor_install(&tcfg, &s_mcu_temp) != ESP_OK ||
        temperature_sensor_enable(s_mcu_temp) != ESP_OK) {
        printf("power_ctrl: MCU temperature sensor unavailable -- mcu_temp_c stays empty\n");
        s_mcu_temp = NULL;
    }

    s_status_mutex = xSemaphoreCreateMutex();
    if (s_status_mutex == NULL) {
        return false;
    }

    BaseType_t created = xTaskCreatePinnedToCore(poll_task, "power_ctrl", 4096, NULL, 5, NULL, 0);
    return created == pdPASS;
}

void power_ctrl_get_status(power_status_t *out) {
    // power_ctrl_start() returns before creating the mutex when the charger isn't
    // found (no I2C bus / BQ25628 not answering). Callers (dashboard status, the
    // recorder's 1 Hz aux row) must then get "no data" (ok=false), not an assert
    // inside xSemaphoreTake(NULL).
    if (s_status_mutex == NULL) {
        memset(out, 0, sizeof(*out));
        out->mcu_temp_c = NAN; // "unavailable", not a fake 0 degC
        return;
    }
    if (xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        *out = s_status;
        xSemaphoreGive(s_status_mutex);
    } else {
        memset(out, 0, sizeof(*out));
        out->mcu_temp_c = NAN;
    }
}

void power_ctrl_trigger_ship_mode(void) {
    enter_low_power_mode(BQ25628_BATFET_SHIP);
}

void power_ctrl_set_button_mark(bool enable) {
    s_button_mark_enabled = enable;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "btn_mark", enable ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

bool power_ctrl_get_button_mark(void) {
    return s_button_mark_enabled;
}

bool power_ctrl_set_ts_ignore(bool ignore) {
    if (!bq25628_set_ts_ignore(&s_chg, ignore)) {
        return false;
    }
    s_ts_ignored = ignore;

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "ts_ignore", ignore ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
    return true;
}

void power_ctrl_reset_battery_learning(int nominal_mah) {
    if (nominal_mah > 0) {
        s_batt_nominal_mah = nominal_mah;
        nvs_handle_t h;
        if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_i32(h, "batt_nom_mah", s_batt_nominal_mah);
            nvs_commit(h);
            nvs_close(h);
        }
    }
    s_batt_learned_mah = (float)s_batt_nominal_mah;
    s_batt_learn_cycles = 0;
    s_batt_mah_consumed = 0.0f;
    s_batt_avg_discharge_ma = 0.0f;
    s_batt_was_above_low = true;
    persist_battery_learning();
    persist_battery_progress();
}
