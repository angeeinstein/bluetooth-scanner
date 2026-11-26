#include "motion.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "accel_capture.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "recorder.h"
#include "scan_log.h"

#define NVS_NS "bts_motion"

// --- Tuning (best-effort values for a body-worn / hand-carried device) -----------
#define GRAVITY_TAU_S 1.0f      // gravity low-pass time constant
#define AXIS_TAU_S 3.0f         // horizontal covariance averaging
#define STILL_G 0.02f           // activity below this ...
#define STILL_SECONDS 2         // ... for this long = still
#define MOVING_G 0.04f          // activity above this = moving
// Walking = a steady rhythm in the vertical acceleration (normalized
// autocorrelation over the last 4 s), not single peaks: impacts and bounces in
// a pocket or bag make individual peaks unreliable, the repetition is not.
// Tuned on a real 55 min log: walking r = 0.6-0.95 at a 1.0-1.25 s stride,
// driving / handling r ~ 0.2, a board lying still can reach 0.7 on noise
// (hence the activity floor).
#define RHY_WIN 400             // window: 4 s at the ~100 Hz feed
#define RHY_LAG_MIN 30          // shortest period searched: 0.3 s (200 steps/min)
#define RHY_LAG_MAX 160         // longest: 1.6 s (a slow stride = 2 steps)
#define RHY_NLAGS (RHY_LAG_MAX - RHY_LAG_MIN + 1)
#define RHY_STRIDE_LAG 75       // a best period >= 0.75 s is a stride (2 steps) ...
#define RHY_HALF_R 0.4f         // ... as is one whose half repeats at least this well
#define RHY_ON 0.6f             // rhythm needed to start walking ...
#define WALK_ON_G 0.08f         // ... with at least this activity ...
#define RHY_ON_SECONDS 3        // ... for this many seconds in a row
#define RHY_HOLD 0.45f          // walking continues while above this ...
#define WALK_HOLD_G 0.05f       // ... and this activity,
#define RHY_OFF_SECONDS 3       // and ends after this many seconds below
#define WEINBERG_K 0.40f        // step length = K * (peak-to-valley in m/s^2)^(1/4); 0.47 is
                                // the waist-worn value, a pocket or bag swings more
#define G_MPS2 9.80665f

static SemaphoreHandle_t s_mux;
static motion_status_t s_st;
static bool s_log = true;
static int s_range_g = 4;

static uint16_t s_h_act[MOTION_HIST_SECONDS];
static int16_t s_h_spd[MOTION_HIST_SECONDS];
static uint8_t s_h_state[MOTION_HIST_SECONDS];
static uint8_t s_h_cad[MOTION_HIST_SECONDS];
static uint32_t s_h_n = 0; // seconds recorded

const char *motion_state_name(motion_state_t s) {
    switch (s) {
    case MOTION_STILL: return "still";
    case MOTION_MOVING: return "moving";
    case MOTION_WALKING: return "walking";
    default: return "unknown";
    }
}

// --- Processing state (motion task only) -----------------------------------------------

static bool s_init = false;
static int64_t s_prev_t = 0;
static float s_g[3];                      // gravity estimate (g)
static float s_c11 = 0, s_c22 = 0, s_c12 = 0; // horizontal covariance (EMA)

// Second window.
static int64_t s_win_end = 0;
static double s_win_sumsq = 0;
static float s_win_peak = 0;
static uint32_t s_win_n = 0;
static int s_still_secs = 0;

// Rhythm detector. Buffers in PSRAM (allocated in motion_start); NULL = off.
static float *s_vring;                    // vertical dynamic acceleration, last RHY_WIN samples
static float *s_vwin;                     // the window, oldest first, mean removed
static float *s_corr;                     // correlation per lag (RHY_LAG_MIN..RHY_LAG_MAX)
static int s_vring_i = 0, s_vring_n = 0;
static bool s_walking = false;
static int s_rhy_on = 0, s_rhy_off = 0;   // seconds in a row with / without the rhythm
static float s_cadence = 0, s_step_m = 0; // of the last rhythmic second
static double s_steps_f = 0;              // step count with its fraction

// Normalized autocorrelation of the last RHY_WIN vertical samples. Returns the
// best correlation over the searched periods (0 without a full window); sets
// *step_lag to the step period in samples (half the best period if that is a
// stride) and *pv to the mean peak-to-valley per step (g).
static float rhythm(float *step_lag, float *pv) {
    *step_lag = 0;
    *pv = 0;
    if (s_vring == NULL || s_vring_n < RHY_WIN) {
        return 0;
    }
    const int n = RHY_WIN;
    float *w = s_vwin;
    float mean = 0;
    for (int i = 0; i < n; i++) {
        w[i] = s_vring[(s_vring_i + i) % n]; // s_vring_i is the oldest sample
        mean += w[i];
    }
    mean /= (float)n;
    float tot = 0;
    for (int i = 0; i < n; i++) {
        w[i] -= mean;
        tot += w[i] * w[i];
    }
    if (tot < 1e-6f) {
        return 0;
    }
    // Energies of the two overlapping parts: head = w[0 .. n-L-1], tail = w[L .. n-1].
    float head = tot, tail = tot;
    for (int i = 0; i < RHY_LAG_MIN; i++) {
        head -= w[n - 1 - i] * w[n - 1 - i];
        tail -= w[i] * w[i];
    }
    int best = RHY_LAG_MIN;
    float best_r = -1;
    for (int L = RHY_LAG_MIN; L <= RHY_LAG_MAX; L++) {
        float dot = 0;
        for (int i = 0; i + L < n; i++) {
            dot += w[i] * w[i + L];
        }
        float den = sqrtf(fmaxf(head, 0) * fmaxf(tail, 0));
        float r = den > 1e-9f ? dot / den : 0;
        s_corr[L - RHY_LAG_MIN] = r;
        if (r > best_r) {
            best_r = r;
            best = L;
        }
        head -= w[n - 1 - L] * w[n - 1 - L];
        tail -= w[L] * w[L];
    }
    // The strongest repetition is often the stride (left/right steps differ,
    // especially in a pocket): halve it when it is too slow for one step or
    // when its half repeats well too.
    float lag = (float)best;
    int h = best / 2;
    if (h >= RHY_LAG_MIN) {
        float rh = -1;
        for (int L = h - 5; L <= h + 5; L++) {
            if (L >= RHY_LAG_MIN && s_corr[L - RHY_LAG_MIN] > rh) rh = s_corr[L - RHY_LAG_MIN];
        }
        if (best >= RHY_STRIDE_LAG || rh >= RHY_HALF_R) {
            lag = (float)best / 2.0f;
        }
    }
    // Peak-to-valley per step-long chunk of the window, averaged.
    int p = (int)(lag + 0.5f);
    float sum = 0;
    int cnt = 0;
    for (int k = 0; k + p <= n; k += p) {
        float lo = w[k], hi = w[k];
        for (int i = k + 1; i < k + p; i++) {
            if (w[i] < lo) lo = w[i];
            if (w[i] > hi) hi = w[i];
        }
        sum += hi - lo;
        cnt++;
    }
    *step_lag = lag;
    *pv = cnt ? sum / (float)cnt : 0;
    return best_r;
}

// Once per second: walking state from the rhythm; steps and distance accrue
// from the cadence while walking. Returns the rhythm (0..1) for the status.
static float update_walking(float act, float *steps_added) {
    float step_lag, pv;
    float r = rhythm(&step_lag, &pv);
    bool rhythmic = step_lag > 0 && r >= (s_walking ? RHY_HOLD : RHY_ON) &&
                    act >= (s_walking ? WALK_HOLD_G : WALK_ON_G);
    float cad = 0, len = 0;
    if (rhythmic) {
        float rate = accel_capture_get_motion_rate_hz();
        if (rate < 50.0f || rate > 200.0f) rate = 100.0f;
        cad = 60.0f * rate / step_lag;
        len = WEINBERG_K * powf(pv * G_MPS2, 0.25f);
        if (len < 0.3f) len = 0.3f;
        if (len > 1.4f) len = 1.4f;
    }
    float add = 0;
    if (!s_walking) {
        if (!rhythmic) {
            s_rhy_on = 0;
        } else if (++s_rhy_on >= RHY_ON_SECONDS) {
            s_walking = true;
            s_rhy_off = 0;
            s_cadence = cad;
            s_step_m = len;
            add = (float)(RHY_ON_SECONDS - 1) * cad / 60.0f; // the seconds it took to confirm
        }
    } else if (rhythmic) {
        s_rhy_off = 0;
        s_cadence = cad;
        s_step_m = len;
    } else if (++s_rhy_off >= RHY_OFF_SECONDS) {
        s_walking = false;
        s_rhy_on = 0;
    }
    if (s_walking) {
        add += s_cadence / 60.0f;
    }
    *steps_added = add;
    return r > 0 ? r : 0;
}

// Once per second: state, cadence, speed, axis; publish + history + MOTION row.
static void finish_second(int64_t now) {
    float act = s_win_n ? (float)sqrt(s_win_sumsq / s_win_n) : 0;
    float peak = s_win_peak;
    s_win_sumsq = 0;
    s_win_peak = 0;
    s_win_n = 0;

    float steps_added;
    float rhy = update_walking(act, &steps_added);
    bool walking = s_walking;
    float cadence = walking ? s_cadence : 0;
    float step_m = walking ? s_step_m : 0;
    s_steps_f += steps_added;

    motion_state_t state;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    state = s_st.state;
    xSemaphoreGive(s_mux);
    if (act < STILL_G) {
        s_still_secs++;
    } else {
        s_still_secs = 0;
    }
    if (walking) {
        state = MOTION_WALKING;
    } else if (s_still_secs >= STILL_SECONDS) {
        state = MOTION_STILL;
    } else if (act > MOVING_G || state == MOTION_WALKING) {
        state = MOTION_MOVING; // moving, or the walking rhythm just ended
    } // else: between the thresholds -- keep the previous state

    float speed = walking ? step_m * cadence / 60.0f : (state == MOTION_STILL ? 0 : -1);

    float axis = -1, conf = 0;
    float tr = s_c11 + s_c22;
    if (state != MOTION_STILL && tr > 1e-4f) {
        float th = 0.5f * atan2f(2 * s_c12, s_c11 - s_c22) * 180.0f / (float)M_PI;
        if (th < 0) th += 180.0f;
        axis = th;
        conf = sqrtf((s_c11 - s_c22) * (s_c11 - s_c22) + 4 * s_c12 * s_c12) / tr;
    }
    float gm = sqrtf(s_g[0] * s_g[0] + s_g[1] * s_g[1] + s_g[2] * s_g[2]);
    float gx = gm > 0.3f ? s_g[0] / gm : 0, gy = gm > 0.3f ? s_g[1] / gm : 0, gz = gm > 0.3f ? s_g[2] / gm : 0;
    // "Up" is opposite to the measured gravity reaction: at rest the sensor reads
    // +1 g on the axis pointing up, so tilt is the angle between +Z and that.
    float tilt = acosf(fmaxf(-1.0f, fminf(1.0f, gz))) * 180.0f / (float)M_PI;

    motion_status_t copy;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_st.ok = true;
    s_st.state = state;
    s_st.steps = (uint32_t)s_steps_f;
    s_st.dist_m += steps_added * step_m;
    s_st.rhythm = rhy;
    s_st.rate_hz = accel_capture_get_motion_rate_hz();
    s_st.activity_g = act;
    s_st.peak_g = peak;
    s_st.cadence_spm = cadence;
    s_st.step_m = step_m;
    s_st.speed_mps = speed;
    s_st.axis_deg = axis;
    s_st.axis_conf = conf;
    s_st.tilt_deg = tilt;
    s_st.gx = gx;
    s_st.gy = gy;
    s_st.gz = gz;
    s_st.dropped = accel_capture_get_ring_drops();
    uint32_t h = s_h_n % MOTION_HIST_SECONDS;
    s_h_act[h] = act * 1000 > 65535 ? 65535 : (uint16_t)(act * 1000);
    s_h_spd[h] = speed < 0 ? -1 : (int16_t)(speed * 100);
    s_h_state[h] = (uint8_t)state;
    s_h_cad[h] = cadence > 255 ? 255 : (uint8_t)cadence;
    s_h_n++;
    copy = s_st;
    xSemaphoreGive(s_mux);

    scan_log_motion_row(&copy, now);
}

static void process(const accel_motion_sample_t *m) {
    float a[3] = {m->x, m->y, m->z};
    if (!s_init) {
        memcpy(s_g, a, sizeof(s_g));
        s_prev_t = m->t_us;
        s_win_end = m->t_us + 1000000;
        s_init = true;
    }
    float dt = (float)(m->t_us - s_prev_t) / 1e6f;
    s_prev_t = m->t_us;
    if (dt <= 0 || dt > 0.2f) { // gap (overrun) or first sample
        dt = 0.01f;
        s_vring_n = 0;          // the rhythm window must be contiguous
    }

    float ag = dt / (GRAVITY_TAU_S + dt);
    for (int i = 0; i < 3; i++) s_g[i] += ag * (a[i] - s_g[i]);
    float gm = sqrtf(s_g[0] * s_g[0] + s_g[1] * s_g[1] + s_g[2] * s_g[2]);
    float u[3] = {0, 0, 1};
    if (gm > 0.3f) {
        for (int i = 0; i < 3; i++) u[i] = s_g[i] / gm;
    }
    float lin[3] = {a[0] - s_g[0], a[1] - s_g[1], a[2] - s_g[2]};
    float linmag2 = lin[0] * lin[0] + lin[1] * lin[1] + lin[2] * lin[2];
    s_win_sumsq += linmag2;
    s_win_n++;
    float lm = sqrtf(linmag2);
    if (lm > s_win_peak) s_win_peak = lm;

    // Vertical dynamic acceleration -> rhythm window.
    if (s_vring != NULL) {
        s_vring[s_vring_i] = lin[0] * u[0] + lin[1] * u[1] + lin[2] * u[2];
        s_vring_i = (s_vring_i + 1) % RHY_WIN;
        if (s_vring_n < RHY_WIN) s_vring_n++;
    }

    // Horizontal plane basis: e1 = device X (or Y if X is nearly vertical)
    // projected onto the plane, e2 = up x e1.
    float ref[3] = {1, 0, 0};
    if (fabsf(u[0]) > 0.9f) {
        ref[0] = 0;
        ref[1] = 1;
    }
    float d = ref[0] * u[0] + ref[1] * u[1] + ref[2] * u[2];
    float e1[3] = {ref[0] - d * u[0], ref[1] - d * u[1], ref[2] - d * u[2]};
    float n1 = sqrtf(e1[0] * e1[0] + e1[1] * e1[1] + e1[2] * e1[2]);
    if (n1 > 1e-3f) {
        for (int i = 0; i < 3; i++) e1[i] /= n1;
        float e2[3] = {u[1] * e1[2] - u[2] * e1[1], u[2] * e1[0] - u[0] * e1[2], u[0] * e1[1] - u[1] * e1[0]};
        float h1 = lin[0] * e1[0] + lin[1] * e1[1] + lin[2] * e1[2];
        float h2 = lin[0] * e2[0] + lin[1] * e2[1] + lin[2] * e2[2];
        float ac = dt / (AXIS_TAU_S + dt);
        s_c11 += ac * (h1 * h1 - s_c11);
        s_c22 += ac * (h2 * h2 - s_c22);
        s_c12 += ac * (h1 * h2 - s_c12);
    }

    if (recorder_is_logging_motion()) {
        char line[64];
        int n = snprintf(line, sizeof(line), "%lld,%.4f,%.4f,%.4f\n", (long long)(m->t_us / 1000), m->x, m->y,
                         m->z);
        if (n > 0 && n < (int)sizeof(line)) {
            recorder_log_motion(line, (size_t)n);
        }
    }

    while (m->t_us >= s_win_end) {
        finish_second(s_win_end);
        s_win_end += 1000000;
    }
}

static void motion_task(void *arg) {
    (void)arg;
    static accel_motion_sample_t buf[32];
    for (;;) {
        size_t n = accel_capture_read_motion(buf, 32);
        if (n == 0) {
            vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        }
        for (size_t i = 0; i < n; i++) {
            process(&buf[i]);
        }
    }
}

// --- Public ---------------------------------------------------------------------------

void motion_get_status(motion_status_t *out) {
    if (s_mux == NULL) {
        memset(out, 0, sizeof(*out));
        out->axis_deg = -1;
        out->speed_mps = -1;
        return;
    }
    xSemaphoreTake(s_mux, portMAX_DELAY);
    *out = s_st;
    xSemaphoreGive(s_mux);
}

int motion_get_history(uint16_t *activity_mg, int16_t *speed_cms, uint8_t *state, uint8_t *cadence, int max) {
    if (s_mux == NULL) {
        return 0;
    }
    xSemaphoreTake(s_mux, portMAX_DELAY);
    int n = s_h_n < MOTION_HIST_SECONDS ? (int)s_h_n : MOTION_HIST_SECONDS;
    if (n > max) n = max;
    for (int i = 0; i < n; i++) {
        uint32_t idx = (s_h_n - (uint32_t)n + (uint32_t)i) % MOTION_HIST_SECONDS;
        activity_mg[i] = s_h_act[idx];
        speed_cms[i] = s_h_spd[idx];
        state[i] = s_h_state[idx];
        cadence[i] = s_h_cad[idx];
    }
    xSemaphoreGive(s_mux);
    return n;
}

static iis3dwb_fs_t fs_for_g(int g) {
    switch (g) {
    case 2: return IIS3DWB_FS_2G;
    case 8: return IIS3DWB_FS_8G;
    case 16: return IIS3DWB_FS_16G;
    default: return IIS3DWB_FS_4G;
    }
}

static void save_u8(const char *key, uint8_t v) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, key, v);
        nvs_commit(h);
        nvs_close(h);
    }
}

bool motion_log_enabled(void) {
    return s_log;
}

void motion_set_log_enabled(bool on) {
    s_log = on;
    save_u8("log", on);
}

int motion_get_range_g(void) {
    return s_range_g;
}

bool motion_set_range_g(int g) {
    if (g != 2 && g != 4 && g != 8 && g != 16) {
        return false;
    }
    // Transient: never written to VibraShock's accel_cfg. The filter stays off
    // -- a high-pass would remove gravity, which tilt and steps depend on.
    if (!accel_capture_set_config_transient(fs_for_g(g), IIS3DWB_FILTER_OFF, IIS3DWB_HPCF_ODR_4)) {
        return false;
    }
    s_range_g = g;
    save_u8("fs", (uint8_t)g);
    return true;
}

void motion_start(void) {
    s_mux = xSemaphoreCreateMutex();
    memset(&s_st, 0, sizeof(s_st));
    s_st.axis_deg = -1;
    s_st.speed_mps = -1;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t v;
        if (nvs_get_u8(h, "log", &v) == ESP_OK) s_log = v != 0;
        if (nvs_get_u8(h, "fs", &v) == ESP_OK && (v == 2 || v == 4 || v == 8 || v == 16)) s_range_g = v;
        nvs_close(h);
    }
    if (!accel_capture_is_ready()) {
        printf("motion: accelerometer not available -- no motion profile\n");
        return;
    }
    // Override whatever VibraShock may have remembered (range/filter) without
    // persisting anything to its namespace.
    accel_capture_set_config_transient(fs_for_g(s_range_g), IIS3DWB_FILTER_OFF, IIS3DWB_HPCF_ODR_4);
    // Rhythm window + work copy + correlations (~3.7 KB) in PSRAM.
    float *rb = heap_caps_calloc(2 * RHY_WIN + RHY_NLAGS, sizeof(float), MALLOC_CAP_SPIRAM);
    if (rb != NULL) {
        s_vwin = rb + RHY_WIN;
        s_corr = rb + 2 * RHY_WIN;
        s_vring = rb; // last: process() checks this one
    } else {
        printf("motion: no memory for the rhythm window -- no walking detection\n");
    }
    // Core 1 at priority 7: below the SD writer (12) and BLE processing (8);
    // the 10 s motion ring absorbs any delay. Stack in PSRAM (internal RAM is
    // scarce); allowed because this task never writes flash.
    if (xTaskCreatePinnedToCoreWithCaps(motion_task, "motion", 4096, NULL, 7, NULL, 1, MALLOC_CAP_SPIRAM) !=
        pdPASS) {
        printf("motion: could not start task\n");
    }
}
