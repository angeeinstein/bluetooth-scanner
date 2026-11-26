#include "led_indicator.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "accel_capture.h"
#include "esp_timer.h"
#include "led_rgb.h"
#include "power_ctrl.h"
#include "recorder.h"
#include "wifi_manager.h"

// ============================================================================
// Colour palette -- named, and separate from any use, so re-hueing later is a
// one-line change. Values are the user's selection (0..255 per channel).
// ============================================================================
typedef struct {
    uint8_t r, g, b;
} rgb_t;

static const rgb_t COL_OFF = {0, 0, 0};
static const rgb_t COL_WHITE = {255, 255, 255};
static const rgb_t COL_BLUE = {0, 0, 255};
static const rgb_t COL_RED = {255, 0, 0};
static const rgb_t COL_GREEN = {0, 255, 0};
static const rgb_t COL_CYAN = {0, 255, 255};
static const rgb_t COL_AMBER = {255, 127, 0};
// Defined for future use (not mapped to a state yet):
static const rgb_t COL_MAGENTA __attribute__((unused)) = {255, 0, 255};
static const rgb_t COL_VIOLET __attribute__((unused)) = {100, 0, 255};
static const rgb_t COL_ROSE __attribute__((unused)) = {255, 50, 50};

// ============================================================================
// Tunables -- timings and thresholds, all in one place.
// ============================================================================
#define WARN_THRESHOLD_S 2700 // start the "recording will stop" ramp at 45 min left
#define WARN_FLASH_ON_MS 100  // fixed, always-perceptible blip for the ramp
#define WARN_GAP_SLOW_MS 8000 // gap at the start of the window (subtle)
#define WARN_GAP_FAST_MS 250  // gap near the end (near-continuous), 0 = solid at t=0
#define WARN_BLIP_PERIOD_MS 4000 // how often the blue "still recording" blip shows
#define WARN_BLIP_LEN_MS 350
#define BATT_WARN_PERIOD_MS 3000 // low-battery warning: gap between the red pulses
#define BATT_WARN_ON_MS 120      // low-battery warning: length of each red pulse

#define REC_BREATHE_MS 2500 // recording blue breathe period
#define REC_BREATHE_MIN 0.05f // dim end of the recording breathe (darker = more visible swing)
#define ERR_BLINK_PERIOD_MS 900 // error red blink (a touch quicker than a "warning")
#define ERR_BLINK_ON_MS 400
#define ERROR_SHOW_MS 8000 // show an error this long after it (re)occurs, then stop -- just
                           // long enough to notice "that didn't work", not so long it looks ongoing

#define EV_ON_MS 90       // default event flash on-time
#define EV_WIFI_ON_MS 450 // longer, so the one-off "Wi-Fi connected" cyan flash is easy to catch
#define EV_GAP_MS 130     // gap between flashes within one event
#define EV_QUEUE_MAX 6

// ============================================================================
// Low-level emit
// ============================================================================
static inline void emit(rgb_t c) {
    led_rgb_set(c.r, c.g, c.b);
}
static void emit_scaled(rgb_t c, float bri) {
    if (bri <= 0) {
        led_rgb_set(0, 0, 0);
        return;
    }
    if (bri > 1) {
        bri = 1;
    }
    led_rgb_set((uint8_t)(c.r * bri), (uint8_t)(c.g * bri), (uint8_t)(c.b * bri));
}

static inline int64_t now_ms(void) {
    return esp_timer_get_time() / 1000;
}

// --- animation helpers ------------------------------------------------------
static float anim_breathe(int period_ms, float min_bri) {
    int64_t ph = now_ms() % period_ms;
    float s = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * (float)ph / (float)period_ms);
    return min_bri + (1.0f - min_bri) * s;
}
static bool anim_blink(int period_ms, int on_ms) {
    return (now_ms() % period_ms) < on_ms;
}
// Fixed flash + gap that shrinks as progress 0->1; progress>=1 = solid.
static bool anim_ramp(int on_ms, int gap_slow, int gap_fast, float progress) {
    if (progress >= 1.0f) {
        return true;
    }
    if (progress < 0) {
        progress = 0;
    }
    int gap = (int)(gap_slow + (gap_fast - gap_slow) * progress);
    int period = on_ms + gap;
    return (now_ms() % period) < on_ms;
}
static void anim_rainbow(void) {
    int hue = (int)((now_ms() / 3) % 360); // ~one rainbow per ~1.1 s
    // integer HSV (s=v=full-ish), value kept modest
    int region = (hue / 60) % 6;
    int rem = (hue % 60) * 255 / 60;
    uint8_t v = 70, p = 0;
    uint8_t q = (uint8_t)(v * (255 - rem) / 255);
    uint8_t t = (uint8_t)(v * rem / 255);
    rgb_t c;
    switch (region) {
        case 0: c = (rgb_t){v, t, p}; break;
        case 1: c = (rgb_t){q, v, p}; break;
        case 2: c = (rgb_t){p, v, t}; break;
        case 3: c = (rgb_t){p, q, v}; break;
        case 4: c = (rgb_t){t, p, v}; break;
        default: c = (rgb_t){v, p, q}; break;
    }
    emit(c);
}

// ============================================================================
// Event flashes (one-shot confirmations) -- a FIFO queue, played one at a time
// to completion so two never overlap or cut each other short.
// ============================================================================
static struct {
    rgb_t col;
    int count;
    int on_ms; // per-event flash on-time (some events want a longer, easier-to-see pulse)
} s_ev_q[EV_QUEUE_MAX];
static int s_ev_q_len = 0;
static struct {
    bool active;
    rgb_t col;
    int count;
    int on_ms;
    int64_t start_ms;
} s_ev;

static void event_enqueue(rgb_t col, int count, int on_ms) {
    if (s_ev_q_len < EV_QUEUE_MAX) {
        s_ev_q[s_ev_q_len].col = col;
        s_ev_q[s_ev_q_len].count = count;
        s_ev_q[s_ev_q_len].on_ms = on_ms;
        s_ev_q_len++;
    }
}
// Returns true while an event owns the LED (so the base is skipped this tick).
static bool event_render(void) {
    if (!s_ev.active) {
        if (s_ev_q_len == 0) {
            return false;
        }
        s_ev.active = true;
        s_ev.col = s_ev_q[0].col;
        s_ev.count = s_ev_q[0].count;
        s_ev.on_ms = s_ev_q[0].on_ms;
        s_ev.start_ms = now_ms();
        for (int i = 1; i < s_ev_q_len; i++) {
            s_ev_q[i - 1] = s_ev_q[i];
        }
        s_ev_q_len--;
    }
    int elapsed = (int)(now_ms() - s_ev.start_ms);
    int cycle = s_ev.on_ms + EV_GAP_MS;
    if (elapsed >= s_ev.count * cycle) {
        s_ev.active = false;
        return s_ev_q_len > 0; // hand straight to the next queued event, if any
    }
    emit((elapsed % cycle) < s_ev.on_ms ? s_ev.col : COL_OFF);
    return true;
}

// ============================================================================
// Test colour (temporary) + edge-detection state
// ============================================================================
static bool s_test_active = false;
static rgb_t s_test_col;

static bool s_seeded = false; // ignore edges on the very first tick
static bool s_prev_rec = false;
static int s_prev_marks = 0;
static accel_selftest_state_t s_prev_st = ACCEL_SELFTEST_IDLE;
static bool s_prev_wifi = false;

void led_ind_set_test(bool active, uint8_t r, uint8_t g, uint8_t b) {
    s_test_active = active;
    s_test_col = (rgb_t){r, g, b};
}

void led_ind_init(void) {
    s_seeded = false;
    s_ev_q_len = 0;
    s_ev.active = false;
    s_test_active = false;
}

// ============================================================================
// State reads
// ============================================================================
// Error is shown as a red blink for a bounded window after it (re)occurs, then
// stops -- a one-off "no SD, tried to record" shouldn't blink for hours. A
// repeat of the same error advances the recorder's timestamp and re-triggers;
// an ongoing hardware fault (missing sensor) is windowed from when first seen.
static int64_t s_err_win_start = 0; // ms; 0 = no window running
static int64_t s_err_rec_at = 0;    // last recorder error timestamp we reacted to

static bool error_active(void) {
    bool has_err = recorder_get_last_error()[0] != '\0'; // a concrete storage/recording fault
    bool has_hw = !accel_capture_is_ready();             // sensor missing (SD-not-in while idle isn't an error)
    if (!has_err && !has_hw) {
        s_err_win_start = 0;
        s_err_rec_at = 0;
        return false;
    }
    if (has_err) {
        int64_t at = recorder_last_error_at_us();
        if (at != s_err_rec_at) { // fresh or repeated error -> (re)start the window
            s_err_rec_at = at;
            s_err_win_start = now_ms();
        }
    }
    if (s_err_win_start == 0) { // e.g. a hardware fault with no recorder timestamp
        s_err_win_start = now_ms();
    }
    return (now_ms() - s_err_win_start) < ERROR_SHOW_MS;
}

// Seconds until the current recording runs out of SD space, or -1 if not
// recording / not known. (The low-battery warning below is a separate, steady
// signal -- it doesn't ramp, since there's no forced shutdown deadline to ramp
// toward; the device runs to the hard voltage cutoff.)
static int sd_stop_in_s(void) {
    if (!recorder_is_recording()) {
        return -1;
    }
    uint64_t total = 0, freeb = 0;
    if (!recorder_get_sd_space(&total, &freeb)) {
        return -1;
    }
    float rate = recorder_get_current_sample_rate_hz();
    if (rate <= 0) {
        return -1;
    }
    double bps = (double)rate * 6.0; // 3 axes * int16
    return bps > 0 ? (int)((double)freeb / bps) : -1;
}

// Fire one-shot events for state edges (start/stop/marker/self-test/Wi-Fi).
static void detect_events(void) {
    bool rec = recorder_is_recording();
    int marks = recorder_marker_count();
    accel_selftest_result_t st;
    accel_capture_self_test_get_result(&st);
    wifi_status_t w;
    wifi_manager_get_status(&w);
    bool wifi = (w.sta_state == WIFI_STA_CONNECTED);

    if (s_seeded) {
        if (rec && !s_prev_rec) {
            event_enqueue(COL_WHITE, 2, EV_ON_MS); // started
        }
        if (!rec && s_prev_rec) {
            event_enqueue(COL_WHITE, 3, EV_ON_MS); // stopped
        }
        if (rec && marks > s_prev_marks) {
            event_enqueue(COL_WHITE, 1, EV_ON_MS); // marker placed
        }
        if (st.state != s_prev_st) {
            if (st.state == ACCEL_SELFTEST_PASS) {
                event_enqueue(COL_GREEN, 2, EV_ON_MS);
            } else if (st.state == ACCEL_SELFTEST_FAIL) {
                event_enqueue(COL_RED, 3, EV_ON_MS);
            }
        }
        if (wifi && !s_prev_wifi) {
            event_enqueue(COL_CYAN, 1, EV_WIFI_ON_MS); // (re)connected -- longer, easier to catch
        }
    }
    s_prev_rec = rec;
    s_prev_marks = marks;
    s_prev_st = st.state;
    s_prev_wifi = wifi;
    s_seeded = true;
}

// A real status is showing -> a lingering colour-test override is cancelled,
// and won't resume afterwards (the user asked for exactly this).
static void cancel_test(void) {
    s_test_active = false;
}

// ============================================================================
// Main tick
// ============================================================================
void led_ind_tick(bool hold_active, bool hold_on, bool party) {
    detect_events();

    // Event flashes win over everything (brief, and confirm real actions).
    if (event_render()) {
        return;
    }

    // Base layer, highest priority first.
    if (hold_active) { // POWER-button ship-hold: white, poll_task owns the blink phase
        cancel_test();
        emit_scaled(COL_WHITE, hold_on ? 1.0f : 0.0f);
        return;
    }
    if (party) {
        cancel_test();
        anim_rainbow();
        return;
    }
    if (error_active()) {
        cancel_test();
        emit_scaled(COL_RED, anim_blink(ERR_BLINK_PERIOD_MS, ERR_BLINK_ON_MS) ? 1.0f : 0.0f);
        return;
    }

    // Low-battery warning -- informational, voltage-based (set once VBAT is at
    // or below the warning threshold on battery). A steady, unobtrusive red
    // pulse: there's no forced shutdown deadline to ramp toward, the device
    // just runs to the hard cutoff. Shown whether idle OR recording (a blue
    // blip breaks through while recording). Outranks the SD warning: an empty
    // battery ends everything, not just the recording.
    power_status_t pw;
    power_ctrl_get_status(&pw);
    if (pw.ok && pw.low_warning) {
        cancel_test();
        if (recorder_is_recording() && (now_ms() % WARN_BLIP_PERIOD_MS) < WARN_BLIP_LEN_MS) {
            emit_scaled(COL_BLUE, anim_breathe(WARN_BLIP_LEN_MS * 2, 0.2f));
        } else {
            emit_scaled(COL_RED, (now_ms() % BATT_WARN_PERIOD_MS) < BATT_WARN_ON_MS ? 1.0f : 0.0f);
        }
        return;
    }

    // Recording running out of SD space -> amber accelerating ramp (only while
    // recording; the blue "still recording" blip breaks through periodically).
    int sd_stop = sd_stop_in_s();
    if (sd_stop >= 0 && sd_stop < WARN_THRESHOLD_S) {
        cancel_test();
        if ((now_ms() % WARN_BLIP_PERIOD_MS) < WARN_BLIP_LEN_MS) {
            emit_scaled(COL_BLUE, anim_breathe(WARN_BLIP_LEN_MS * 2, 0.2f));
        } else {
            float progress = 1.0f - (float)sd_stop / (float)WARN_THRESHOLD_S;
            emit_scaled(COL_AMBER, anim_ramp(WARN_FLASH_ON_MS, WARN_GAP_SLOW_MS, WARN_GAP_FAST_MS, progress) ? 1.0f : 0.0f);
        }
        return;
    }

    if (recorder_is_recording()) { // recording, all good
        cancel_test();
        emit_scaled(COL_BLUE, anim_breathe(REC_BREATHE_MS, REC_BREATHE_MIN));
        return;
    }

    if (s_test_active) { // idle, but the colour tester is holding a colour
        emit(s_test_col);
        return;
    }

    emit(COL_OFF); // idle
}
