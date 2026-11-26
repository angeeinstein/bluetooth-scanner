#include "accel_capture.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "pins.h"

#define ACCEL_NVS_NS "accel_cfg"

#define ACCEL_SPI_HOST SPI2_HOST

// How many entries we ask the sensor to buffer before raising the watermark
// interrupt, and the matching size of our burst-read scratch buffer. Half
// the 512-deep FIFO: ~9.6ms to drain before the other half fills too.
#define FIFO_WATERMARK 256
#define FIFO_MAX_TRANSFER_BYTES (1 + IIS3DWB_FIFO_DEPTH * IIS3DWB_FIFO_ENTRY_BYTES)

// Raw sample rate is fixed at 26.7kHz; decimate 250:1 for a ~106.8Hz,
// boxcar-averaged preview feed for the web dashboard's live graph. A lower
// factor means a higher preview rate and less averaging (the boxcar's -3dB
// corner scales with it -- ~48Hz at 250:1), at negligible CPU/network cost;
// it does not change the number of WS clients the server can hold.
#define PREVIEW_DECIMATION 250
#define PREVIEW_RING_CAPACITY (256 * sizeof(accel_preview_point_t))

// Motion feed for motion.c: boxcar average of MOTION_DECIMATION raw samples
// -> 26.667 kHz / 267 = ~99.9 Hz, each with the esp_timer time of its centre.
// The boxcar's first null sits at the output rate, a decent anti-alias filter
// for body motion (well below ~20 Hz). 1024 entries = ~10 s of headroom.
#define MOTION_DECIMATION 267
#define MOTION_RING_CAPACITY (1024 * sizeof(accel_motion_sample_t))

// Full-rate (undecimated) stream ring for udp_stream.c -- much smaller than
// the recording ring since dropping is expected/fine for a live stream
// (~1.6s of headroom at ~160KB/s to absorb Wi-Fi jitter before dropping).
#define STREAM_RING_CAPACITY (256 * 1024)

// Same size/rationale as STREAM_RING_CAPACITY, for ws_stream.c's binary
// WebSocket stream -- a separate ring so it can be independently drained
// regardless of whether the UDP stream is active (see accel_capture.h).
#define WS_STREAM_RING_CAPACITY (256 * 1024)

static iis3dwb_t s_accel;
static bool s_accel_ready = false;
static iis3dwb_fs_t s_fs = IIS3DWB_FS_2G;
static iis3dwb_filter_mode_t s_filter = IIS3DWB_FILTER_OFF;
static iis3dwb_hpcf_t s_hpcf = IIS3DWB_HPCF_ODR_4;
static bool s_remember_config = false; // persist/restore fs+filter across reboots (opt-in)

static ring_buffer_t s_motion_ring;
static ring_buffer_t s_preview_ring;
static ring_buffer_t s_stream_ring;
static ring_buffer_t s_ws_stream_ring;

static uint8_t *s_burst_buf = NULL; // DMA-capable, FIFO_WATERMARK entries worth

static SemaphoreHandle_t s_fifo_sem;
static TaskHandle_t s_drain_task = NULL;
static volatile bool s_paused = false;

// Guards every SPI transaction on s_accel.spi. The drain task polls this
// continuously (many times/sec) via iis3dwb_fifo_status()/burst_read(); a
// config change (accel_capture_set_config(), from the dashboard's HTTP
// handler task) can land on the same device at any time recording isn't
// active. spi_device_polling_transmit() (used for single-register reads/
// writes) is not safe for concurrent use from two tasks -- without this,
// that collision fails with "Cannot send polling transaction while the
// previous polling transaction is not terminated", which then looks
// identical to the FIFO getting stuck (the read fails, nothing drains, the
// FIFO fills up unread).
static SemaphoreHandle_t s_spi_mutex;

static volatile uint32_t s_sensor_overruns = 0;
static volatile uint32_t s_ring_drops = 0;
static volatile uint32_t s_stream_ring_drops = 0;
static volatile uint32_t s_ws_stream_ring_drops = 0;
static esp_timer_handle_t s_startup_ovr_timer;
// Full-scale-saturation (clipping) tally -- a total plus per-axis counts, diffed
// into a live percentage by accel_capture_get_clip_pct(). See CLIP_THRESHOLD.
static volatile uint32_t s_clip_total = 0;
static volatile uint32_t s_clip_count[3] = {0, 0, 0};

// Actual-ODR / sensor-timestamp measurement (see accel_capture_get_measured_odr_hz()).
// s_samples_total counts every sample pulled from the FIFO since boot; the sensor
// timestamp counter (read in the drain task) is the sensor's own clock. Comparing
// the two -- against the reliable esp_timer for absolute seconds -- yields the true
// ODR (from clean windows) and the sensor tick period. All updated in the drain
// task; the volatiles below are the published snapshot (aligned 32-bit == atomic).
static volatile uint64_t s_samples_total = 0;
static volatile uint32_t s_last_ts_ticks = 0;   // most recent raw sensor timestamp
static volatile float s_measured_odr_hz = 0.0f;  // cumulative true ODR, 0 until first clean window
static volatile float s_ts_tick_us = 0.0f;       // EMA sensor tick period, microseconds
static volatile uint32_t s_overrun_lost_samples = 0; // estimated samples lost to FIFO overruns
// Drain-task-only estimator state: previous sample point + the lifetime clean
// baseline (summed samples/us over overrun-free windows) behind s_measured_odr_hz.
static bool s_odr_have_prev = false;
static uint32_t s_odr_prev_ts = 0;
static uint64_t s_odr_prev_samples = 0;
static int64_t s_odr_prev_esp_us = 0;
static uint32_t s_odr_prev_overruns = 0;
static uint64_t s_odr_clean_samples = 0;
static int64_t s_odr_clean_us = 0;

static float s_decim_sum_x = 0, s_decim_sum_y = 0, s_decim_sum_z = 0;
static uint32_t s_decim_count = 0;


// Built-in self-test (see accel_capture_self_test_start()). Datasheet pass
// window is 800..3200 mg per axis with FS forced to +-4g.
#define SELFTEST_MIN_MG 800.0f
#define SELFTEST_MAX_MG 3200.0f
#define SELFTEST_SETTLE_MS 60 // let the output settle after each config/ST change
#define SELFTEST_SAMPLES 64   // averaged per phase to knock down noise
static volatile accel_selftest_state_t s_selftest_state = ACCEL_SELFTEST_IDLE;
static accel_selftest_result_t s_selftest_result;

static void IRAM_ATTR fifo_isr_handler(void *arg) {
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_fifo_sem, &woken);
    if (woken) {
        portYIELD_FROM_ISR();
    }
}

// A raw code within this margin of the 16-bit rail (+-32767) counts the axis as
// full-scale saturated -- clipping. ~99.5% of full scale, so it also catches a
// signal riding the rail (which bounces a few LSB below the exact maximum).
#define CLIP_THRESHOLD 32600

// Motion boxcar state (drain task only).
static float s_mot_sum_x = 0, s_mot_sum_y = 0, s_mot_sum_z = 0;
static int s_mot_count = 0;
static int64_t s_mot_t_first = 0;

// `t_us` = esp_timer time this sample was taken (estimated from the FIFO
// backlog at read time and the measured ODR -- see accel_drain_task).
static void process_entry(const uint8_t *entry, int64_t t_us) {
    // entry[0] = tag (unused -- we only ever batch XL data); entry[1..6] =
    // X_L,X_H,Y_L,Y_H,Z_L,Z_H.
    int16_t raw_x = (int16_t)((entry[2] << 8) | entry[1]);
    int16_t raw_y = (int16_t)((entry[4] << 8) | entry[3]);
    int16_t raw_z = (int16_t)((entry[6] << 8) | entry[5]);

    // Clipping (full-scale saturation) tally -- 3 compares per sample, ~0.1% of
    // a core at 26.7kHz. accel_capture_get_clip_pct() turns these into a live %.
    s_clip_total++;
    s_samples_total++; // samples pulled from the FIFO (see the ODR estimator)
    if (raw_x >= CLIP_THRESHOLD || raw_x <= -CLIP_THRESHOLD) s_clip_count[0]++;
    if (raw_y >= CLIP_THRESHOLD || raw_y <= -CLIP_THRESHOLD) s_clip_count[1]++;
    if (raw_z >= CLIP_THRESHOLD || raw_z <= -CLIP_THRESHOLD) s_clip_count[2]++;

    int16_t triplet[3] = {raw_x, raw_y, raw_z};
    size_t written;

    // Full-rate stream ring for udp_stream.c -- fed unconditionally (like
    // the recording ring above), regardless of whether streaming is
    // currently enabled; the UDP task itself decides whether to send or
    // discard what it drains. Dropping here is expected/fine.
    written = ring_buffer_write(&s_stream_ring, (const uint8_t *)triplet, sizeof(triplet));
    if (written < sizeof(triplet)) {
        s_stream_ring_drops++;
    }

    // Same idiom, second independent ring for ws_stream.c.
    written = ring_buffer_write(&s_ws_stream_ring, (const uint8_t *)triplet, sizeof(triplet));
    if (written < sizeof(triplet)) {
        s_ws_stream_ring_drops++;
    }

    float sens_g = iis3dwb_sensitivity_mg_per_lsb(s_fs) / 1000.0f;

    // ~100 Hz motion feed (g), timestamped at the centre of its boxcar window.
    if (s_mot_count == 0) {
        s_mot_t_first = t_us;
    }
    s_mot_sum_x += raw_x * sens_g;
    s_mot_sum_y += raw_y * sens_g;
    s_mot_sum_z += raw_z * sens_g;
    if (++s_mot_count >= MOTION_DECIMATION) {
        accel_motion_sample_t m = {
            .t_us = s_mot_t_first + (t_us - s_mot_t_first) / 2,
            .x = s_mot_sum_x / MOTION_DECIMATION,
            .y = s_mot_sum_y / MOTION_DECIMATION,
            .z = s_mot_sum_z / MOTION_DECIMATION,
        };
        if (ring_buffer_write(&s_motion_ring, (const uint8_t *)&m, sizeof(m)) < sizeof(m)) {
            s_ring_drops++; // motion.c fell behind (or isn't running)
        }
        s_mot_sum_x = s_mot_sum_y = s_mot_sum_z = 0;
        s_mot_count = 0;
    }

    s_decim_sum_x += raw_x * sens_g;
    s_decim_sum_y += raw_y * sens_g;
    s_decim_sum_z += raw_z * sens_g;
    s_decim_count++;
    if (s_decim_count >= PREVIEW_DECIMATION) {
        accel_preview_point_t pt = {
            .x = s_decim_sum_x / s_decim_count,
            .y = s_decim_sum_y / s_decim_count,
            .z = s_decim_sum_z / s_decim_count,
        };
        // Best-effort: if nobody's draining the preview ring (no dashboard
        // client connected), just drop points -- never worth blocking or
        // losing recording data over.
        ring_buffer_write(&s_preview_ring, (const uint8_t *)&pt, sizeof(pt));
        s_decim_sum_x = s_decim_sum_y = s_decim_sum_z = 0;
        s_decim_count = 0;
    }
}

// Runs once per drain wake, ~2x/second, with the freshly-read sensor timestamp.
// Maintains three published values from ~0.5s measurement windows:
//
//   - Measured true ODR (Hz): a *long-baseline cumulative* average -- every
//     overrun-free window's samples and elapsed microseconds are summed since
//     boot, and the rate is their ratio. This keeps tightening the longer the
//     device runs (error shrinks ~1/sqrt(time)), far more accurate than a short
//     sliding window. The esp_timer (crystal-accurate) is the reference, since
//     "Hz" means real SI seconds; overrun windows are excluded so the sample
//     count reflects true production, not what we happened to read.
//   - Estimated overrun sample loss: over a window that saw a new overrun, the
//     samples we *should* have captured (ODR x elapsed) minus what we did =
//     roughly how many the FIFO overwrote. A magnitude, unlike the raw event
//     count. Approximate (a few samples of noise), which is all it needs to be.
//   - Sensor tick period (us): a passive cross-check that the sensor clock and
//     esp_timer agree; also converts a raw sensor timestamp to real seconds.
static void update_odr_estimate(uint32_t ts_ticks) {
    uint64_t samples = s_samples_total;
    int64_t esp_us = esp_timer_get_time();
    uint32_t overruns = s_sensor_overruns;
    s_last_ts_ticks = ts_ticks;

    if (!s_odr_have_prev) {
        s_odr_prev_ts = ts_ticks;
        s_odr_prev_samples = samples;
        s_odr_prev_esp_us = esp_us;
        s_odr_prev_overruns = overruns;
        s_odr_have_prev = true;
        return;
    }

    int64_t d_esp = esp_us - s_odr_prev_esp_us;
    if (d_esp < 500000) {
        return; // accumulate at least ~0.5s per update to keep the ratios stable
    }
    uint32_t d_ts = ts_ticks - s_odr_prev_ts; // unsigned wrap-safe
    uint64_t d_samp = samples - s_odr_prev_samples;
    bool clean = (overruns == s_odr_prev_overruns);

    if (clean && d_samp > 0) {
        // Fold this window into the lifetime clean baseline.
        s_odr_clean_samples += d_samp;
        s_odr_clean_us += d_esp;
        if (s_odr_clean_us > 0) {
            s_measured_odr_hz =
                (float)((double)s_odr_clean_samples * 1e6 / (double)s_odr_clean_us);
        }
        if (d_ts > 0) {
            float tick_us = (float)d_esp / (float)d_ts; // sensor tick period, real microseconds
            s_ts_tick_us = (s_ts_tick_us > 0.0f) ? (0.9f * s_ts_tick_us + 0.1f * tick_us) : tick_us;
        }
    } else if (!clean && s_measured_odr_hz > 0.0f) {
        // Window with an overrun: estimate how many samples were overwritten as
        // (expected from the known ODR) - (actually captured). Only once the ODR
        // baseline exists; guard against a negative (drain catching up a backlog).
        double expected = (double)s_measured_odr_hz * (double)d_esp / 1e6;
        if (expected > (double)d_samp) {
            s_overrun_lost_samples += (uint32_t)(expected - (double)d_samp + 0.5);
        }
    }

    s_odr_prev_ts = ts_ticks;
    s_odr_prev_samples = samples;
    s_odr_prev_esp_us = esp_us;
    s_odr_prev_overruns = overruns;
}

static void accel_drain_task(void *arg) {
    for (;;) {
        xSemaphoreTake(s_fifo_sem, portMAX_DELAY);

        if (s_paused) {
            // Cooperative pause (see accel_capture_pause()): skip touching
            // the SPI bus entirely and go back to waiting. Never used
            // vTaskSuspend here -- suspending this task while it's inside a
            // blocking spi_device_transmit() call would leave the SPI
            // driver's internal transaction state stuck, hanging every
            // burst read after resume.
            continue;
        }

        // Drain until caught up, not just once per interrupt: if a backlog
        // ever exceeds one burst's capacity, the watermark line may not
        // produce another rising edge on its own while we're behind it, so
        // we keep pulling until FIFO_STATUS reports nothing left rather
        // than waiting for a interrupt that might not come.
        xSemaphoreTake(s_spi_mutex, portMAX_DELAY);
        for (;;) {
            uint16_t num_entries = 0;
            bool overrun = false;
            if (!iis3dwb_fifo_status(&s_accel, &num_entries, &overrun)) {
                break;
            }
            if (overrun) {
                s_sensor_overruns++;
            }
            if (num_entries == 0) {
                break;
            }

            uint16_t to_read = (num_entries > FIFO_WATERMARK) ? FIFO_WATERMARK : num_entries;
            // The newest FIFO entry was sampled ~now; entry i of the backlog is
            // (num_entries - 1 - i) sample periods older.
            int64_t t_read = esp_timer_get_time();
            float odr = s_measured_odr_hz > 1000.0f ? s_measured_odr_hz : 26667.0f;
            float period_us = 1e6f / odr;
            if (!iis3dwb_fifo_burst_read(&s_accel, to_read, s_burst_buf)) {
                break;
            }
            for (uint16_t i = 0; i < to_read; i++) {
                int64_t t = t_read - (int64_t)((float)(num_entries - 1 - i) * period_us);
                process_entry(s_burst_buf + (size_t)i * IIS3DWB_FIFO_ENTRY_BYTES, t);
            }

            if (to_read == num_entries) {
                break; // caught up
            }
            // else more remains -- loop immediately without waiting for another interrupt
        }
        // Sample the sensor's own timestamp once we're caught up (still holding
        // the SPI mutex), then feed the ODR/tick estimator outside the lock.
        uint32_t ts_ticks = 0;
        bool ts_ok = iis3dwb_read_timestamp(&s_accel, &ts_ticks);
        xSemaphoreGive(s_spi_mutex);
        if (ts_ok) {
            update_odr_estimate(ts_ticks);
        }
    }
}

static bool init_spi_and_sensor(void) {
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_ACCEL_MOSI,
        .miso_io_num = PIN_ACCEL_MISO,
        .sclk_io_num = PIN_ACCEL_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = FIFO_MAX_TRANSFER_BYTES,
    };
    if (spi_bus_initialize(ACCEL_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO) != ESP_OK) {
        return false;
    }
    if (!iis3dwb_init(ACCEL_SPI_HOST, &s_accel)) {
        return false;
    }

    uint8_t who_am_i = 0;
    if (!iis3dwb_read_reg(&s_accel, IIS3DWB_REG_WHO_AM_I, &who_am_i) ||
        who_am_i != IIS3DWB_WHO_AM_I_VAL) {
        printf("accel_capture: WHO_AM_I mismatch (got 0x%02X, expected 0x%02X)\n", who_am_i,
               IIS3DWB_WHO_AM_I_VAL);
        return false;
    }
    return true;
}

// Enables sampling and routes the FIFO watermark flag to INT1. Must only be
// called after the GPIO interrupt is already configured and armed (see the
// ordering note in accel_capture_start()) -- the FIFO fills to its 512-entry
// watermark within ~19ms of this being called, and if nothing is listening
// for the resulting rising edge yet, that edge is lost for good (nothing
// ever drains the FIFO back below the watermark to produce a second one).
static bool start_sampling(void) {
    if (!iis3dwb_configure(&s_accel, s_fs, s_filter, s_hpcf)) {
        return false;
    }
    // Enable the free-running timestamp counter so the drain task can measure the
    // true ODR and give recorded samples a hardware time reference. Non-fatal:
    // capture still works without it, we just lose the ODR/timestamp readout.
    iis3dwb_enable_timestamp(&s_accel, true);
    if (!iis3dwb_fifo_configure(&s_accel, FIFO_WATERMARK)) {
        return false;
    }
    if (!iis3dwb_fifo_enable_watermark_int(&s_accel)) {
        return false;
    }
    return true;
}

static void persist_accel_config(void) {
    nvs_handle_t h;
    if (nvs_open(ACCEL_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "fs", (uint8_t)s_fs);
        nvs_set_u8(h, "filter", (uint8_t)s_filter);
        nvs_set_u8(h, "hpcf", (uint8_t)s_hpcf);
        nvs_commit(h);
        nvs_close(h);
    }
}

// Loads the persisted accelerometer config at boot -- but only overrides the
// compiled-in defaults (s_fs/s_filter/s_hpcf) when "remember" was enabled. Call
// before start_sampling() so the sensor comes up already configured.
static void load_accel_config(void) {
    nvs_handle_t h;
    if (nvs_open(ACCEL_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return; // never saved -- keep the +-2g / filter-off defaults
    }
    uint8_t remember = 0;
    nvs_get_u8(h, "remember", &remember);
    s_remember_config = remember != 0;
    if (s_remember_config) {
        uint8_t fs = 0, filter = 0, hpcf = 0, lpf2 = 0;
        if (nvs_get_u8(h, "fs", &fs) == ESP_OK && fs <= (uint8_t)IIS3DWB_FS_8G) {
            s_fs = (iis3dwb_fs_t)fs;
        }
        if (nvs_get_u8(h, "filter", &filter) == ESP_OK &&
            filter <= (uint8_t)IIS3DWB_FILTER_HIGHPASS) {
            s_filter = (iis3dwb_filter_mode_t)filter;
        } else if (nvs_get_u8(h, "lpf2", &lpf2) == ESP_OK) {
            // Migrate an older build's on/off-only setting (LPF2 low-pass, ODR/4).
            s_filter = lpf2 ? IIS3DWB_FILTER_LOWPASS : IIS3DWB_FILTER_OFF;
        }
        if (nvs_get_u8(h, "hpcf", &hpcf) == ESP_OK && hpcf <= (uint8_t)IIS3DWB_HPCF_ODR_800) {
            s_hpcf = (iis3dwb_hpcf_t)hpcf;
        }
    }
    nvs_close(h);
}

// One-shot fired ~0.5s after sampling starts. A freshly-enabled FIFO fills to
// its watermark within ~10ms, and a couple of overruns are normal in the brief
// handoff before the drain task is steadily keeping up. Zero the counter once
// so the live figure reflects only real, post-startup loss. Recordings log the
// counter at start and stop, so their per-recording deltas are unaffected.
static void clear_startup_overruns_cb(void *arg) {
    (void)arg;
    s_sensor_overruns = 0;
    s_overrun_lost_samples = 0; // keep the loss estimate on the same baseline
}

bool accel_capture_start(void) {
    s_burst_buf = heap_caps_malloc(FIFO_MAX_TRANSFER_BYTES, MALLOC_CAP_DMA);
    if (s_burst_buf == NULL) {
        printf("accel_capture: failed to allocate burst buffer\n");
        return false;
    }

    if (!ring_buffer_init(&s_motion_ring, MOTION_RING_CAPACITY, MALLOC_CAP_SPIRAM)) {
        printf("accel_capture: failed to allocate motion ring (PSRAM)\n");
        return false;
    }
    if (!ring_buffer_init(&s_preview_ring, PREVIEW_RING_CAPACITY, MALLOC_CAP_DEFAULT)) {
        printf("accel_capture: failed to allocate preview ring\n");
        return false;
    }
    if (!ring_buffer_init(&s_stream_ring, STREAM_RING_CAPACITY, MALLOC_CAP_SPIRAM)) {
        printf("accel_capture: failed to allocate stream ring (PSRAM)\n");
        return false;
    }
    if (!ring_buffer_init(&s_ws_stream_ring, WS_STREAM_RING_CAPACITY, MALLOC_CAP_SPIRAM)) {
        printf("accel_capture: failed to allocate WS stream ring (PSRAM)\n");
        return false;
    }

    s_fifo_sem = xSemaphoreCreateBinary();
    if (s_fifo_sem == NULL) {
        return false;
    }
    s_spi_mutex = xSemaphoreCreateMutex();
    if (s_spi_mutex == NULL) {
        return false;
    }

    if (!init_spi_and_sensor()) {
        return false;
    }

    // GPIO interrupt must be configured and armed *before* start_sampling()
    // enables the FIFO watermark routing below -- otherwise the FIFO can
    // already be past the watermark (INT1 already physically high) by the
    // time we start listening for a rising edge, and since nothing has
    // drained it yet, that edge never re-occurs. This was a real bug: the
    // drain task never woke a single time, while a direct FIFO_STATUS poll
    // showed the sensor sampling fine and the FIFO stuck full at 512/512
    // with INT1 permanently high.
    gpio_config_t int_conf = {
        .pin_bit_mask = 1ULL << PIN_ACCEL_INT1,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_POSEDGE,
    };
    gpio_config(&int_conf);

    static bool isr_service_installed = false;
    if (!isr_service_installed) {
        if (gpio_install_isr_service(0) != ESP_OK) {
            printf("accel_capture: gpio_install_isr_service failed\n");
            return false;
        }
        isr_service_installed = true;
    }
    if (gpio_isr_handler_add(PIN_ACCEL_INT1, fifo_isr_handler, NULL) != ESP_OK) {
        return false;
    }
    BaseType_t created = xTaskCreatePinnedToCore(accel_drain_task, "accel_drain", 4096, NULL, 20,
                                                  &s_drain_task, 1);
    if (created != pdPASS) {
        return false;
    }

    load_accel_config(); // restore persisted fs/lpf2 if "remember" was enabled

    if (!start_sampling()) {
        return false;
    }

    // Suppress the benign startup FIFO overruns from the reported counter (see
    // clear_startup_overruns_cb). Best-effort: if the timer can't be created the
    // only cost is the live counter showing the 1-2 boot overruns, as before.
    const esp_timer_create_args_t ovr_timer_args = {
        .callback = clear_startup_overruns_cb,
        .name = "startup_ovr_reset",
    };
    if (esp_timer_create(&ovr_timer_args, &s_startup_ovr_timer) == ESP_OK) {
        esp_timer_start_once(s_startup_ovr_timer, 500000); // 0.5s
    }

    s_accel_ready = true;
    return true;
}

bool accel_capture_is_ready(void) {
    return s_accel_ready;
}

void accel_capture_pause(void) {
    s_paused = true;
}

void accel_capture_resume(void) {
    s_paused = false;
    // The FIFO is in STREAM mode, so while paused it just keeps overwriting
    // its oldest entries once full rather than re-triggering the watermark
    // line -- there may be no future rising edge to wake the drain task on
    // its own. Give the semaphore ourselves to force a drain pass.
    if (s_fifo_sem != NULL) {
        xSemaphoreGive(s_fifo_sem);
    }
}

static bool set_config_impl(iis3dwb_fs_t fs, iis3dwb_filter_mode_t filter, iis3dwb_hpcf_t cutoff,
                            bool persist) {
    xSemaphoreTake(s_spi_mutex, portMAX_DELAY);
    bool ok = iis3dwb_configure(&s_accel, fs, filter, cutoff);
    xSemaphoreGive(s_spi_mutex);
    if (!ok) {
        return false;
    }
    s_fs = fs;
    s_filter = filter;
    s_hpcf = cutoff;
    if (persist) {
        persist_accel_config();
    }
    return true;
}

bool accel_capture_set_config(iis3dwb_fs_t fs, iis3dwb_filter_mode_t filter,
                              iis3dwb_hpcf_t cutoff) {
    return set_config_impl(fs, filter, cutoff, s_remember_config);
}

bool accel_capture_set_config_transient(iis3dwb_fs_t fs, iis3dwb_filter_mode_t filter,
                                        iis3dwb_hpcf_t cutoff) {
    // Never persists, even when "remember" is on -- so an automatic (auto-range)
    // change doesn't overwrite the user's manually chosen, remembered preference.
    return set_config_impl(fs, filter, cutoff, false);
}

iis3dwb_fs_t accel_capture_get_fs(void) {
    return s_fs;
}

iis3dwb_filter_mode_t accel_capture_get_filter_mode(void) {
    return s_filter;
}

iis3dwb_hpcf_t accel_capture_get_hpcf(void) {
    return s_hpcf;
}

bool accel_capture_get_lpf2(void) {
    return s_filter != IIS3DWB_FILTER_OFF;
}

bool accel_capture_get_remember_config(void) {
    return s_remember_config;
}

void accel_capture_set_remember_config(bool remember) {
    s_remember_config = remember;
    nvs_handle_t h;
    if (nvs_open(ACCEL_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "remember", remember ? 1 : 0);
        if (remember) {
            // Snapshot the current config so it's what gets restored next boot.
            nvs_set_u8(h, "fs", (uint8_t)s_fs);
            nvs_set_u8(h, "filter", (uint8_t)s_filter);
            nvs_set_u8(h, "hpcf", (uint8_t)s_hpcf);
        }
        nvs_commit(h);
        nvs_close(h);
    }
}

float accel_capture_get_sensitivity_mg_per_lsb(void) {
    return iis3dwb_sensitivity_mg_per_lsb(s_fs);
}

bool accel_capture_read_temp_c(float *out_c) {
    if (!s_accel_ready) {
        return false;
    }
    xSemaphoreTake(s_spi_mutex, portMAX_DELAY);
    bool ok = iis3dwb_read_temp_c(&s_accel, out_c);
    xSemaphoreGive(s_spi_mutex);
    return ok;
}

// Averages SELFTEST_SAMPLES direct output-register reads (in g, at +-4g).
// Caller holds s_spi_mutex. Returns false on any SPI failure.
static bool selftest_average(float *ax, float *ay, float *az) {
    float sx = 0, sy = 0, sz = 0;
    for (int i = 0; i < SELFTEST_SAMPLES; i++) {
        float x, y, z;
        if (!iis3dwb_read_xyz_g(&s_accel, IIS3DWB_FS_4G, &x, &y, &z)) {
            return false;
        }
        sx += x;
        sy += y;
        sz += z;
    }
    *ax = sx / SELFTEST_SAMPLES;
    *ay = sy / SELFTEST_SAMPLES;
    *az = sz / SELFTEST_SAMPLES;
    return true;
}

static void self_test_task(void *arg) {
    iis3dwb_fs_t saved_fs = s_fs;
    iis3dwb_filter_mode_t saved_filter = s_filter;
    iis3dwb_hpcf_t saved_hpcf = s_hpcf;
    accel_selftest_result_t r = {.state = ACCEL_SELFTEST_RUNNING};

    // Take exclusive ownership of the SPI bus: pause the drain task so it
    // stops initiating passes, then acquire the mutex (which waits out any
    // pass already in flight). From here the sensor is ours until we resume.
    s_paused = true;
    xSemaphoreTake(s_spi_mutex, portMAX_DELAY);

    float offx = 0, offy = 0, offz = 0, onx = 0, ony = 0, onz = 0;
    bool ok = iis3dwb_configure(&s_accel, IIS3DWB_FS_4G, IIS3DWB_FILTER_OFF, IIS3DWB_HPCF_ODR_4) &&
              iis3dwb_write_reg(&s_accel, IIS3DWB_REG_CTRL5_C, IIS3DWB_ST_XL_OFF);
    if (ok) {
        vTaskDelay(pdMS_TO_TICKS(SELFTEST_SETTLE_MS));
        ok = selftest_average(&offx, &offy, &offz);
    }
    if (ok) {
        ok = iis3dwb_write_reg(&s_accel, IIS3DWB_REG_CTRL5_C, IIS3DWB_ST_XL_POSITIVE);
    }
    if (ok) {
        vTaskDelay(pdMS_TO_TICKS(SELFTEST_SETTLE_MS));
        ok = selftest_average(&onx, &ony, &onz);
    }

    // Always restore, regardless of ok: self-test off, the user's FS/filter
    // back, and a fresh FIFO (flush stale entries produced during the test +
    // re-arm the watermark interrupt), mirroring start_sampling()'s sequence.
    iis3dwb_write_reg(&s_accel, IIS3DWB_REG_CTRL5_C, IIS3DWB_ST_XL_OFF);
    iis3dwb_configure(&s_accel, saved_fs, saved_filter, saved_hpcf);
    iis3dwb_fifo_configure(&s_accel, FIFO_WATERMARK);
    iis3dwb_fifo_enable_watermark_int(&s_accel);

    xSemaphoreGive(s_spi_mutex);
    s_paused = false;
    xSemaphoreGive(s_fifo_sem); // kick the drain task back into action

    if (ok) {
        r.change_mg_x = fabsf(onx - offx) * 1000.0f;
        r.change_mg_y = fabsf(ony - offy) * 1000.0f;
        r.change_mg_z = fabsf(onz - offz) * 1000.0f;
        bool pass = r.change_mg_x >= SELFTEST_MIN_MG && r.change_mg_x <= SELFTEST_MAX_MG &&
                    r.change_mg_y >= SELFTEST_MIN_MG && r.change_mg_y <= SELFTEST_MAX_MG &&
                    r.change_mg_z >= SELFTEST_MIN_MG && r.change_mg_z <= SELFTEST_MAX_MG;
        r.state = pass ? ACCEL_SELFTEST_PASS : ACCEL_SELFTEST_FAIL;
    } else {
        r.state = ACCEL_SELFTEST_FAIL; // SPI failure mid-test
    }
    s_selftest_result = r;
    s_selftest_state = r.state;
    vTaskDelete(NULL);
}

bool accel_capture_self_test_start(void) {
    if (!s_accel_ready || s_selftest_state == ACCEL_SELFTEST_RUNNING) {
        return false;
    }
    memset(&s_selftest_result, 0, sizeof(s_selftest_result));
    s_selftest_result.state = ACCEL_SELFTEST_RUNNING;
    s_selftest_state = ACCEL_SELFTEST_RUNNING;
    BaseType_t created =
        xTaskCreatePinnedToCore(self_test_task, "accel_st", 4096, NULL, 10, NULL, 1);
    if (created != pdPASS) {
        s_selftest_state = ACCEL_SELFTEST_IDLE;
        return false;
    }
    return true;
}

void accel_capture_self_test_get_result(accel_selftest_result_t *out) {
    *out = s_selftest_result;
    out->state = s_selftest_state;
}

size_t accel_capture_read_motion(accel_motion_sample_t *out, size_t max_samples) {
    size_t got = ring_buffer_read(&s_motion_ring, (uint8_t *)out, max_samples * sizeof(*out));
    return got / sizeof(*out);
}

float accel_capture_get_motion_rate_hz(void) {
    float odr = s_measured_odr_hz > 1000.0f ? s_measured_odr_hz : 26667.0f;
    return odr / MOTION_DECIMATION;
}

ring_buffer_t *accel_capture_get_stream_ring(void) {
    return &s_stream_ring;
}

ring_buffer_t *accel_capture_get_ws_stream_ring(void) {
    return &s_ws_stream_ring;
}

size_t accel_capture_read_preview(accel_preview_point_t *out, size_t max_points) {
    size_t max_bytes = max_points * sizeof(accel_preview_point_t);
    size_t read_bytes = ring_buffer_read(&s_preview_ring, (uint8_t *)out, max_bytes);
    return read_bytes / sizeof(accel_preview_point_t);
}

uint32_t accel_capture_get_sensor_overruns(void) {
    return s_sensor_overruns;
}

uint32_t accel_capture_get_ring_drops(void) {
    return s_ring_drops;
}

uint32_t accel_capture_get_stream_ring_drops(void) {
    return s_stream_ring_drops;
}

uint32_t accel_capture_get_ws_stream_ring_drops(void) {
    return s_ws_stream_ring_drops;
}


float accel_capture_get_measured_odr_hz(void) {
    return s_measured_odr_hz;
}

uint64_t accel_capture_get_samples_total(void) {
    return s_samples_total;
}

uint32_t accel_capture_get_overrun_lost_samples(void) {
    return s_overrun_lost_samples;
}

void accel_capture_get_time_snapshot(uint32_t *ts_ticks_out, uint64_t *samples_out,
                                     float *tick_us_out) {
    if (ts_ticks_out) *ts_ticks_out = s_last_ts_ticks;
    if (samples_out) *samples_out = s_samples_total;
    if (tick_us_out) *tick_us_out = s_ts_tick_us;
}

void accel_capture_get_clip_pct(float out[3]) {
    // Per-axis percentage of samples at/near full-scale saturation since the
    // previous call. Free-running monotonic counters diffed against the last
    // read -- lock-free (aligned 32-bit reads are atomic) and meant for a single
    // ~1Hz caller (the status poll), giving a ~1s rolling window. On a very short
    // interval (rapid re-poll from several clients) it returns the last figure
    // rather than a noisy count over a handful of samples.
    static uint32_t last_total = 0, last_clip[3] = {0, 0, 0};
    static float last_pct[3] = {0.0f, 0.0f, 0.0f};
    uint32_t total = s_clip_total;
    uint32_t d_total = total - last_total;
    if (d_total < 2000) { // < ~75ms of samples -- keep the previous window's value
        out[0] = last_pct[0];
        out[1] = last_pct[1];
        out[2] = last_pct[2];
        return;
    }
    last_total = total;
    for (int i = 0; i < 3; i++) {
        uint32_t c = s_clip_count[i];
        uint32_t d = c - last_clip[i];
        last_clip[i] = c;
        last_pct[i] = 100.0f * (float)d / (float)d_total;
        out[i] = last_pct[i];
    }
}

void accel_capture_get_clip_counts(uint32_t *total_out, uint32_t out[3]) {
    if (total_out) {
        *total_out = s_clip_total;
    }
    if (out) {
        out[0] = s_clip_count[0];
        out[1] = s_clip_count[1];
        out[2] = s_clip_count[2];
    }
}
