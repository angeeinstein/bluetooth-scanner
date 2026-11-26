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
#define STEP_MIN_G 0.08f        // minimum step peak (dynamic vertical acceleration)
#define STEP_HYST_G 0.05f       // peak is confirmed once the signal falls this far below it
#define STEP_MIN_S 0.28f        // fastest plausible step interval (~214 steps/min)
#define STEP_MAX_S 2.0f         // slower than this breaks the rhythm
#define STEP_RUN 3              // steps in a row before they count
#define WEINBERG_K 0.47f        // step length = K * (peak-to-valley in m/s^2)^(1/4)
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
static float s_ma[5];                     // vertical moving average
static int s_ma_i = 0;
static float s_c11 = 0, s_c22 = 0, s_c12 = 0; // horizontal covariance (EMA)

// Second window.
static int64_t s_win_end = 0;
static double s_win_sumsq = 0;
static float s_win_peak = 0;
static uint32_t s_win_n = 0;
static int s_still_secs = 0;

// Step detector.
static bool s_can_peak = false;
static float s_peak = -1e9f, s_valley = 1e9f;
static int64_t s_peak_t = 0;
static int64_t s_last_step_t = 0;
static float s_amp_avg = 0.2f;
static int s_run = 0;
static float s_pending_len[STEP_RUN];
static int64_t s_pending_t[STEP_RUN];
static int s_npending = 0;
#define STEP_HIST 16
static int64_t s_step_t[STEP_HIST];
static float s_step_len[STEP_HIST];
static int s_step_i = 0, s_step_count = 0;

static void commit_step(int64_t t, float len) {
    s_step_t[s_step_i] = t;
    s_step_len[s_step_i] = len;
    s_step_i = (s_step_i + 1) % STEP_HIST;
    if (s_step_count < STEP_HIST) s_step_count++;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_st.steps++;
    s_st.dist_m += len;
    xSemaphoreGive(s_mux);
}

static void on_step(int64_t t, float amp) {
    float interval = s_last_step_t ? (float)(t - s_last_step_t) / 1e6f : 1e9f;
    s_last_step_t = t;
    s_amp_avg = 0.8f * s_amp_avg + 0.2f * amp;
    float len = WEINBERG_K * powf(amp * G_MPS2, 0.25f);
    if (len < 0.3f) len = 0.3f;
    if (len > 1.4f) len = 1.4f;
    if (interval >= STEP_MIN_S && interval <= STEP_MAX_S) {
        s_run++;
    } else {
        s_run = 1;
        s_npending = 0;
    }
    if (s_run < STEP_RUN) { // not a rhythm yet -- hold it
        if (s_npending < STEP_RUN) {
            s_pending_t[s_npending] = t;
            s_pending_len[s_npending++] = len;
        }
        return;
    }
    for (int i = 0; i < s_npending; i++) { // rhythm confirmed: count the held steps too
        commit_step(s_pending_t[i], s_pending_len[i]);
    }
    s_npending = 0;
    commit_step(t, len);
}

static void detect_step(float v, int64_t t) {
    if (v < s_valley) s_valley = v;
    if (v < 0) {
        s_can_peak = true; // a new peak needs a dip below 0 g first
    }
    if (!s_can_peak) {
        return;
    }
    if (v > s_peak) {
        s_peak = v;
        s_peak_t = t;
    }
    float thr = fmaxf(STEP_MIN_G, 0.35f * s_amp_avg);
    if (s_peak > thr && v < s_peak - STEP_HYST_G &&
        (s_last_step_t == 0 || (float)(s_peak_t - s_last_step_t) / 1e6f >= STEP_MIN_S)) {
        float amp = s_peak - s_valley;
        on_step(s_peak_t, amp);
        s_can_peak = false;
        s_peak = -1e9f;
        s_valley = v;
    }
}

// Once per second: state, cadence, speed, axis; publish + history + MOTION row.
static void finish_second(int64_t now) {
    float act = s_win_n ? (float)sqrt(s_win_sumsq / s_win_n) : 0;
    float peak = s_win_peak;
    s_win_sumsq = 0;
    s_win_peak = 0;
    s_win_n = 0;

    // Cadence + step length from committed steps in the last 10 s.
    int n10 = 0;
    int64_t first = 0, last = 0;
    float len_sum = 0;
    int len_n = 0;
    for (int i = 0; i < s_step_count; i++) {
        int idx = (s_step_i - 1 - i + STEP_HIST) % STEP_HIST;
        int64_t t = s_step_t[idx];
        if (now - t > 10000000) break;
        if (n10 == 0) last = t;
        first = t;
        n10++;
        if (len_n < 4) {
            len_sum += s_step_len[idx];
            len_n++;
        }
    }
    bool walking = n10 >= 2 && (now - last) <= 2000000;
    float cadence = (walking && last > first) ? (float)(n10 - 1) * 60e6f / (float)(last - first) : 0;
    float step_m = len_n ? len_sum / len_n : 0;

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
        for (int i = 0; i < 5; i++) s_ma[i] = 0;
        s_prev_t = m->t_us;
        s_win_end = m->t_us + 1000000;
        s_init = true;
    }
    float dt = (float)(m->t_us - s_prev_t) / 1e6f;
    s_prev_t = m->t_us;
    if (dt <= 0 || dt > 0.2f) dt = 0.01f; // gap (overrun) or first sample

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

    // Vertical dynamic acceleration, smoothed over 5 samples (~50 ms), -> steps.
    float v = lin[0] * u[0] + lin[1] * u[1] + lin[2] * u[2];
    s_ma[s_ma_i] = v;
    s_ma_i = (s_ma_i + 1) % 5;
    float vs = (s_ma[0] + s_ma[1] + s_ma[2] + s_ma[3] + s_ma[4]) / 5.0f;
    detect_step(vs, m->t_us);

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
    // Core 1 at priority 7: below the SD writer (12) and BLE processing (8);
    // the 10 s motion ring absorbs any delay. Stack in PSRAM (internal RAM is
    // scarce); allowed because this task never writes flash.
    if (xTaskCreatePinnedToCoreWithCaps(motion_task, "motion", 4096, NULL, 7, NULL, 1, MALLOC_CAP_SPIRAM) !=
        pdPASS) {
        printf("motion: could not start task\n");
    }
}
