#include "recorder.h"

#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "accel_capture.h"
#include "device_id.h"
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "pins.h"
#include "power_ctrl.h"
#include "recording_format.h"
#include "wifi_manager.h"
#include "ring_buffer.h"
#include "sdmmc_cmd.h"
#include "version.h"

#define MOUNT_POINT "/sdcard"
#define REC_DIR MOUNT_POINT "/rec"

#define WRITE_CHUNK_SIZE (32 * 1024)
#define ROTATION_THRESHOLD_BYTES (3500ULL * 1024 * 1024) // stay under FAT32's 4GB/file limit

// Sync-checkpoint table sits between the header and the sample data (see
// recording_format.h). DATA_START_OFFSET is where the raw triplets begin.
#define SYNC_TABLE_BYTES ((size_t)SYNC_TABLE_ENTRIES * sizeof(sync_checkpoint_t))
#define DATA_START_OFFSET (sizeof(recording_header_t) + SYNC_TABLE_BYTES)

// --- Auto-range on clipping (see maybe_auto_range / auto_range_rotate) --------
// When enabled, a recording that saturates full scale for a sustained period is
// split into a new rotation part at a larger range, so an unattended recording
// isn't ruined by clipping. Runs headless in the writer task. Escalates up only,
// caps at +-16g; a new part means each .bin keeps a single, self-describing range.
#define AUTORANGE_NVS_NS "recorder"
#define AUTORANGE_CHECK_MS 1000     // evaluate clipping over ~1s windows
#define AUTORANGE_TRIGGER_PCT 0.3f  // any axis clipping >= this % of samples...
#define AUTORANGE_SUSTAIN_MS 5000   // ...continuously for this long triggers a step up
#define AUTORANGE_COOLDOWN_MS 8000  // ignore clipping this long after a change (let it settle)
#define AUTORANGE_PROBE_MS 1500     // measure the true peak at +-16g for this long
#define AUTORANGE_HEADROOM 1.3f     // pick the smallest range fitting peak * this

// Recording-level clip flag for the "clipped" integrity badge -- tracked
// independently of the auto-range toggle, so a manual recording that clips is
// flagged too. Sustained, not a one-off bump: a window must clip >=
// CLIP_FLAG_PCT for CLIP_FLAG_SUSTAIN consecutive ~1s windows to set it.
#define CLIP_FLAG_PCT 0.3f
#define CLIP_FLAG_SUSTAIN 3

// Periodic durability sync. FATFS only commits a file's directory entry (its
// size) and FAT chain to the card on f_sync/f_close, so without this an
// ungraceful stop (power loss, yanked card) would orphan the whole
// in-progress file. Flushing every few seconds bounds worst-case loss to
// this interval; the ~25s recording-ring headroom absorbs the small
// metadata-write latency, so it doesn't cost samples.
#define FSYNC_INTERVAL_MS 5000

// Card-detect debounce: require the raw GPIO level to be stable for this
// many consecutive polls before treating it as a real insert/remove event
// (mechanical switches bounce during insertion/removal).
#define CD_POLL_INTERVAL_MS 200
#define CD_DEBOUNCE_POLLS 5 // ~1s stable

typedef enum {
    REC_IDLE,
    REC_RECORDING,
    REC_STOP_REQUESTED,
} rec_state_t;

static bool s_sd_mounted = false;
static sdmmc_card_t *s_card = NULL;

static volatile rec_state_t s_state = REC_IDLE;
static volatile bool s_start_requested = false;
static char s_pending_tag[64] = "";
static bool s_pending_decimated = false;
static char s_current_tag[64] = "";
static bool s_current_decimated = false;
static int s_rotation_part = 1;
// The recording's shared filename base ("<ts>_<tag>" or "rec_<seq>_<tag>"),
// computed once for part 1 and reused by every rotation part -- so all parts
// share a base and the dashboard groups them into one recording. Regenerating
// it per part (a fresh timestamp/sequence) is what previously orphaned part 2+.
static char s_recording_base[100] = "";

// Auto-range-on-clipping state. s_auto_range is the persisted enable; the rest
// is writer-task-only monitor state (see maybe_auto_range).
static bool s_auto_range = false;
static int64_t s_ar_check_us = 0;
static uint32_t s_ar_last_total = 0, s_ar_last_clip[3] = {0, 0, 0};
static int64_t s_ar_high_since_us = 0; // when sustained clipping began (0 = not high)
static int64_t s_ar_cooldown_until_us = 0;

// Recording-level clip tracking (writer-task-only; see update_clip_flag).
// Cumulative across rotation parts; reset per recording.
static float s_rec_clip_max_pct = 0.0f;
static int s_rec_clip_run = 0;
static bool s_rec_clip_flag = false;
static int64_t s_rec_clip_check_us = 0;
static uint32_t s_rec_clip_last_total = 0, s_rec_clip_last[3] = {0, 0, 0};

static FILE *s_file = NULL;
static FILE *s_sidecar = NULL;

// Aux side-channels: one CSV row per second of slow-changing values (sensor/board/
// charger temperatures, power, Wi-Fi signal, data-loss counters) in <base>.aux.csv
// next to each part's .bin. A separate text file rather than inside the .bin, so
// the sample stream and its format stay untouched; rows are keyed by sample_index
// (time = sample_index / header.sample_rate_hz), like the sync checkpoints. Every
// value is a cached status copy or a short register read -- negligible next to the
// 160 KB/s sample stream (~130 B/s). See ARCHITECTURE.md / README "aux file".
#define AUX_INTERVAL_US 1000000
static FILE *s_aux = NULL;
static int64_t s_aux_last_us = 0;
static uint32_t s_aux_lost_base = 0;   // overrun-lost samples at part start (column is per part)
static uint32_t s_aux_drops_base = 0;  // file-ring drops at part start
static uint32_t s_aux_clip_last[3] = {0, 0, 0}; // per-axis clip counts at the previous row
static char s_current_filename[160] = "";
static volatile uint64_t s_bytes_written = 0; // total file bytes: header + sync table + data

// Sync-checkpoint table for the current part (see recording_format.h). Collected
// in RAM and flushed into the file's reserved table region on each fsync.
static sync_checkpoint_t s_sync[SYNC_TABLE_ENTRIES];
static uint32_t s_sync_count = 0;    // checkpoints latched this part
static uint32_t s_sync_flushed = 0;  // of those, how many are on disk
static int64_t s_sync_last_us = 0;   // esp_timer at the last checkpoint
static char s_last_error[64] = "";
static int64_t s_last_error_at_us = 0; // when s_last_error was last (re)set -- lets the
                                       // status LED show an error briefly, then stop
// A benign, informational stop message (e.g. the card filled up) -- shown neutrally
// on the dashboard, not as an error. Cleared when a new recording starts.
static char s_last_notice[64] = "";
static int64_t s_last_fsync_us = 0;
static int64_t s_file_start_us = 0; // when the current file opened -- for the .log dur_s field

// Markers for the current recording (see recorder.h). Guarded by
// s_marker_mutex, which is taken by the web/button tasks that add/edit/remove
// marks and by the writer task when it clears them at a new recording's start.
#define MAX_MARKERS 64
static recorder_marker_t s_markers[MAX_MARKERS];
static int s_marker_count = 0;
static uint32_t s_next_marker_id = 1;
static SemaphoreHandle_t s_marker_mutex = NULL;
static int64_t s_recording_start_us = 0; // recording start (persists across rotation parts)
static char s_marker_path[176] = "";     // <base>.mark for the current recording

static SemaphoreHandle_t s_stop_done_sem;

static float fs_to_g_value(iis3dwb_fs_t fs) {
    switch (fs) {
        case IIS3DWB_FS_2G: return 2.0f;
        case IIS3DWB_FS_4G: return 4.0f;
        case IIS3DWB_FS_8G: return 8.0f;
        case IIS3DWB_FS_16G: return 16.0f;
        default: return 2.0f;
    }
}

static void sanitize_tag(const char *in, char *out, size_t out_len) {
    size_t oi = 0;
    if (in != NULL) {
        for (size_t i = 0; in[i] != '\0' && oi + 1 < out_len; i++) {
            char c = in[i];
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                c == '-' || c == '_') {
                out[oi++] = c;
            } else if (c == ' ') {
                out[oi++] = '-';
            }
            // else: silently drop characters FAT32 filenames can't hold safely
        }
    }
    out[oi] = '\0';
}

static bool wallclock_is_valid(struct tm *out_tm) {
    time_t now = time(NULL);
    localtime_r(&now, out_tm);
    // Before NTP sync, the system clock reads near the 1970 epoch; treat
    // anything before 2020 as "not synced yet".
    return (out_tm->tm_year + 1900) >= 2020;
}

static uint32_t next_sequence_number(void) {
    uint32_t max_seq = 0;
    DIR *d = opendir(REC_DIR);
    if (d != NULL) {
        struct dirent *entry;
        while ((entry = readdir(d)) != NULL) {
            unsigned int seq;
            if (sscanf(entry->d_name, "rec_%u", &seq) == 1 && seq > max_seq) {
                max_seq = seq;
            }
        }
        closedir(d);
    }
    return max_seq + 1;
}

static void build_filename(const char *tag, int part, char *out, size_t out_len) {
    // Part 1 computes the shared base and remembers it; every later rotation
    // part reuses it and only appends "_partN", so all parts of one recording
    // share a base name and the dashboard groups + aggregates them.
    if (part <= 1) {
        char safe_tag[64];
        sanitize_tag(tag, safe_tag, sizeof(safe_tag));
        struct tm tm_info;
        if (wallclock_is_valid(&tm_info)) {
            char ts[24];
            strftime(ts, sizeof(ts), "%Y-%m-%d_%H%M%S", &tm_info);
            if (safe_tag[0] != '\0') {
                snprintf(s_recording_base, sizeof(s_recording_base), "%s_%s", ts, safe_tag);
            } else {
                snprintf(s_recording_base, sizeof(s_recording_base), "%s", ts);
            }
        } else {
            uint32_t seq = next_sequence_number();
            if (safe_tag[0] != '\0') {
                snprintf(s_recording_base, sizeof(s_recording_base), "rec_%05u_%s", (unsigned)seq,
                         safe_tag);
            } else {
                snprintf(s_recording_base, sizeof(s_recording_base), "rec_%05u", (unsigned)seq);
            }
        }
        snprintf(out, out_len, REC_DIR "/%s.bin", s_recording_base);
    } else {
        snprintf(out, out_len, REC_DIR "/%s_part%d.bin", s_recording_base, part);
    }
}

static char *sidecar_path_for(const char *bin_path, char *out, size_t out_len) {
    snprintf(out, out_len, "%s", bin_path);
    size_t len = strlen(out);
    if (len > 4 && strcmp(out + len - 4, ".bin") == 0) {
        strcpy(out + len - 4, ".log");
    }
    return out;
}

// Commits the current file's buffered data + FATFS metadata (directory size,
// FAT chain) to the card, and re-arms the periodic-sync timer. fflush pushes
// the stdio buffer down to FATFS; fsync then forces f_sync. Best-effort --
// a real card loss surfaces on the next fwrite (handled there).
static void sync_flush_checkpoints(void); // defined below; called from sync_file

static void sync_file(void) {
    if (s_file != NULL) {
        fflush(s_file);
        sync_flush_checkpoints(); // persist any new sync checkpoints into the table
        fflush(s_file);
        fsync(fileno(s_file));
        s_last_fsync_us = esp_timer_get_time();
    }
    if (s_aux != NULL) { // same durability window for the 1 Hz aux rows
        fflush(s_aux);
        fsync(fileno(s_aux));
    }
}

// The sample rate stamped into the header and the start-log line. For full-rate
// captures this is the *measured* ODR -- the accelerometer's true output rate,
// derived from the sensor's own timestamp -- rather than the nominal 26.7kHz
// label. A wrong rate is a systematic error that shifts every frequency in
// offline analysis by the same factor, so it's worth storing the real number.
// Falls back to the nominal rate if the estimate isn't ready yet (e.g. a
// recording started within the first second or two of boot).
static float effective_sample_rate_hz(void) {
    if (s_current_decimated) {
        return accel_capture_get_recording_decim_rate_hz();
    }
    float measured = accel_capture_get_measured_odr_hz();
    return (measured > 0.0f) ? measured : 26700.0f;
}

// Latches one sync checkpoint: the shared Wi-Fi TSF now, paired with how many
// samples are already in the file. Called at recording start (sample 0, the
// anchor) and every ~60 s after. RAM only; sync_flush_checkpoints() persists it.
static void sync_add_checkpoint(void) {
    if (s_sync_count >= SYNC_TABLE_ENTRIES) {
        return; // table full -- can't happen within a rotation-limited part
    }
    uint64_t sample_index =
        (s_bytes_written > DATA_START_OFFSET) ? (s_bytes_written - DATA_START_OFFSET) / 6 : 0;
    sync_checkpoint_t *cp = &s_sync[s_sync_count];
    cp->sample_index = sample_index;
    cp->tsf_us = wifi_manager_get_tsf_us();   // 0 if unassociated
    wifi_manager_get_bssid(cp->bssid);        // which AP that TSF belongs to (0 if unassociated)
    cp->reserved[0] = cp->reserved[1] = 0;
    s_sync_count++;
    s_sync_last_us = esp_timer_get_time();
}

// Writes any not-yet-persisted checkpoints into the file's reserved table region
// and bumps sync_count in the header, then returns the write position to the end
// so data keeps appending. Best-effort; the caller must have fflush'd first.
static void sync_flush_checkpoints(void) {
    if (s_file == NULL || s_sync_count == s_sync_flushed) {
        return;
    }
    long table = (long)(sizeof(recording_header_t) + (size_t)s_sync_flushed * sizeof(sync_checkpoint_t));
    if (fseek(s_file, table, SEEK_SET) == 0) {
        fwrite(&s_sync[s_sync_flushed], sizeof(sync_checkpoint_t), s_sync_count - s_sync_flushed, s_file);
    }
    if (fseek(s_file, (long)offsetof(recording_header_t, sync_count), SEEK_SET) == 0) {
        uint32_t c = s_sync_count;
        fwrite(&c, sizeof(c), 1, s_file);
    }
    fseek(s_file, 0, SEEK_END); // restore the append position
    s_sync_flushed = s_sync_count;
}

// Samples dropped before reaching *this* file's ring (full-rate or decimated).
static uint32_t file_ring_drops(void) {
    return s_current_decimated ? accel_capture_get_recording_decim_ring_drops()
                               : accel_capture_get_ring_drops();
}

// Appends one aux row. Unavailable values are left empty (pandas/Excel read NaN).
static void aux_write_row(void) {
    if (s_aux == NULL) {
        return;
    }
    s_aux_last_us = esp_timer_get_time();
    uint64_t sample_index =
        (s_bytes_written > DATA_START_OFFSET) ? (s_bytes_written - DATA_START_OFFSET) / 6 : 0;
    double elapsed_s = (s_file_start_us > 0) ? (double)(s_aux_last_us - s_file_start_us) / 1e6 : 0.0;

    char accel_t[12] = "", mcu_t[12] = "", chg_t[12] = "", vbus[12] = "", vbat[12] = "";
    char ibus[12] = "", ibat[12] = "", pct[8] = "", chg[4] = "", rssi[8] = "";
    float t;
    if (accel_capture_read_temp_c(&t)) {
        snprintf(accel_t, sizeof(accel_t), "%.2f", (double)t);
    }
    power_status_t p;
    power_ctrl_get_status(&p); // cached copy, refreshed ~1 s by power_ctrl's own task
    if (!isnan(p.mcu_temp_c)) { // read on core 0 by power_ctrl -- see power_status_t
        snprintf(mcu_t, sizeof(mcu_t), "%.1f", (double)p.mcu_temp_c);
    }
    if (p.ok) {
        snprintf(chg_t, sizeof(chg_t), "%.1f", (double)p.tdie_c);
        snprintf(vbus, sizeof(vbus), "%.3f", p.vbus_mv / 1000.0);
        snprintf(vbat, sizeof(vbat), "%.3f", p.vbat_mv / 1000.0);
        snprintf(ibus, sizeof(ibus), "%d", p.ibus_ma);
        snprintf(ibat, sizeof(ibat), "%d", p.ichg_ma); // + charging, - discharging
        snprintf(chg, sizeof(chg), "%d", p.charging ? 1 : 0);
        if (p.battery_present) {
            snprintf(pct, sizeof(pct), "%.0f", (double)p.batt_percent);
        }
    }
    int8_t r;
    if (wifi_manager_get_rssi(&r)) {
        snprintf(rssi, sizeof(rssi), "%d", r);
    }
    // Per-axis full-scale counts (the clip "total" is the all-samples denominator,
    // not a clip count), diffed into "clipped in the last row interval".
    uint32_t clip_now[3];
    accel_capture_get_clip_counts(NULL, clip_now);
    uint32_t clipped[3];
    for (int i = 0; i < 3; i++) {
        clipped[i] = clip_now[i] - s_aux_clip_last[i];
        s_aux_clip_last[i] = clip_now[i];
    }

    fprintf(s_aux, "%llu,%.3f,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%u,%u,%u,%u,%u\n",
            (unsigned long long)sample_index, elapsed_s, accel_t, mcu_t, chg_t, vbus, vbat, ibus, ibat,
            pct, chg, rssi, (unsigned)(accel_capture_get_overrun_lost_samples() - s_aux_lost_base),
            (unsigned)(file_ring_drops() - s_aux_drops_base), (unsigned)clipped[0],
            (unsigned)clipped[1], (unsigned)clipped[2]);
}

// Opens <base>.aux.csv for a new part and writes the column header + the row at
// sample 0. Best-effort: a failure only costs the aux data, never the recording.
static void aux_open(const char *bin_path) {
    char path[168];
    snprintf(path, sizeof(path), "%s", bin_path);
    size_t len = strlen(path);
    if (len > 4 && strcmp(path + len - 4, ".bin") == 0) {
        strcpy(path + len - 4, ".aux.csv");
    } else {
        return;
    }
    s_aux = fopen(path, "w");
    if (s_aux == NULL) {
        printf("recorder: could not open %s (errno=%d) -- recording continues without it\n", path, errno);
        return;
    }
    s_aux_lost_base = accel_capture_get_overrun_lost_samples();
    s_aux_drops_base = file_ring_drops();
    accel_capture_get_clip_counts(NULL, s_aux_clip_last);
    fprintf(s_aux, "sample_index,elapsed_s,accel_temp_c,mcu_temp_c,charger_temp_c,vbus_v,vbat_v,"
                   "ibus_ma,ibat_ma,battery_pct,charging,rssi_dbm,lost_samples,drops,"
                   "clipped_x,clipped_y,clipped_z\n");
    aux_write_row();
}

// Final row at the part's exact end, then close.
static void aux_close(void) {
    if (s_aux != NULL) {
        aux_write_row();
        fclose(s_aux);
        s_aux = NULL;
    }
}

static void write_header(void) {
    recording_header_t hdr = {0};
    memcpy(hdr.magic, RECORDING_MAGIC, 4);
    hdr.version = 3;
    hdr.sample_rate_hz = effective_sample_rate_hz();
    hdr.fs_range_g = fs_to_g_value(accel_capture_get_fs());
    hdr.sensitivity_mg_per_lsb = accel_capture_get_sensitivity_mg_per_lsb();
    hdr.boot_time_us = (int64_t)esp_timer_get_time();
    hdr.sync_table_bytes = (uint32_t)SYNC_TABLE_BYTES;
    hdr.sync_count = 0;
    hdr.sync_interval_s = SYNC_CHECKPOINT_INTERVAL_S;
    wifi_manager_get_bssid(hdr.bssid);

    struct tm tm_info;
    if (wallclock_is_valid(&tm_info)) {
        hdr.wall_time_valid = 1;
        hdr.wall_time_unix_s = (int64_t)time(NULL);
    }

    fwrite(&hdr, sizeof(hdr), 1, s_file);
    // Reserve the (zeroed) sync-checkpoint table between the header and the data.
    uint8_t zeros[256] = {0};
    for (size_t left = SYNC_TABLE_BYTES; left > 0;) {
        size_t w = left < sizeof(zeros) ? left : sizeof(zeros);
        fwrite(zeros, 1, w, s_file);
        left -= w;
    }
    s_bytes_written = DATA_START_OFFSET;
    // Fresh checkpoint state for this part, then latch the start anchor (sample 0).
    s_sync_count = 0;
    s_sync_flushed = 0;
    sync_add_checkpoint();
    sync_file(); // commit the header + table + anchor now, so even a very early
                 // power loss leaves a valid, openable file
}

static bool open_new_file(const char *tag, int part) {
    char path[160];
    build_filename(tag, part, path, sizeof(path));

    s_file = fopen(path, "wb");
    if (s_file == NULL) {
        snprintf(s_last_error, sizeof(s_last_error), "open failed (errno=%d %s)", errno,
                 strerror(errno));
        s_last_error_at_us = esp_timer_get_time();
        printf("recorder: failed to open %s (errno=%d %s)\n", path, errno, strerror(errno));
        return false;
    }
    strncpy(s_current_filename, path, sizeof(s_current_filename) - 1);
    s_current_filename[sizeof(s_current_filename) - 1] = '\0';

    char sidecar_path[160];
    sidecar_path_for(path, sidecar_path, sizeof(sidecar_path));
    s_sidecar = fopen(sidecar_path, "a");

    write_header();
    s_file_start_us = esp_timer_get_time();

    if (s_sidecar != NULL) {
        char tempbuf[16];
        float temp_c = 0.0f;
        if (accel_capture_read_temp_c(&temp_c)) {
            snprintf(tempbuf, sizeof(tempbuf), "%.1f", (double)temp_c);
        } else {
            snprintf(tempbuf, sizeof(tempbuf), "nan");
        }
        char devid[16];
        device_id_str(devid, sizeof(devid));
        iis3dwb_filter_mode_t fm = accel_capture_get_filter_mode();
        const char *filter_str = (fm == IIS3DWB_FILTER_HIGHPASS) ? "highpass"
                                 : (fm == IIS3DWB_FILTER_LOWPASS) ? "lowpass"
                                                                  : "off";
        int filter_hz = (fm == IIS3DWB_FILTER_OFF) ? 0 : iis3dwb_hpcf_cutoff_hz(accel_capture_get_hpcf());
        // dev/fw stamp the recording's provenance -- which board and firmware
        // produced it -- for traceability across multiple field devices. lpf2 is
        // kept (on/off) for older readers; filter/filter_hz describe the exact mode.
        fprintf(s_sidecar,
                "start part=%d dev=%s fw=%s fs_g=%.0f rate_hz=%.1f lpf2=%s filter=%s "
                "filter_hz=%d temp_c=%s overruns=%u drops=%u lost_samples=%u\n",
                part, devid, FIRMWARE_VERSION, (double)fs_to_g_value(accel_capture_get_fs()),
                (double)effective_sample_rate_hz(), accel_capture_get_lpf2() ? "on" : "off",
                filter_str, filter_hz, tempbuf,
                (unsigned)accel_capture_get_sensor_overruns(),
                (unsigned)accel_capture_get_ring_drops(),
                (unsigned)accel_capture_get_overrun_lost_samples());
        fflush(s_sidecar);
        // Commit the start line to the card *now*. FATFS only writes a file's
        // directory-entry size on sync/close, so without this a reset or
        // brownout mid-recording leaves a 0-byte log (losing the settings,
        // provenance, and the "did this end cleanly?" signal). fsync updates
        // the dir entry, so a truncated recording still keeps its start line.
        fsync(fileno(s_sidecar));
    }
    aux_open(path);
    return true;
}

static void close_current_file(const char *reason) {
    aux_close();
    if (s_sidecar != NULL) {
        char tempbuf[16];
        float temp_c = 0.0f;
        if (accel_capture_read_temp_c(&temp_c)) {
            snprintf(tempbuf, sizeof(tempbuf), "%.1f", (double)temp_c);
        } else {
            snprintf(tempbuf, sizeof(tempbuf), "nan");
        }
        float dur_s =
            (s_file_start_us > 0) ? (float)(esp_timer_get_time() - s_file_start_us) / 1e6f : 0.0f;
        fprintf(s_sidecar,
                "%s bytes=%llu dur_s=%.1f temp_c=%s overruns=%u drops=%u lost_samples=%u "
                "rate_hz=%.1f clip_pct=%.2f clipped=%d\n",
                reason, (unsigned long long)s_bytes_written, (double)dur_s, tempbuf,
                (unsigned)accel_capture_get_sensor_overruns(), (unsigned)accel_capture_get_ring_drops(),
                (unsigned)accel_capture_get_overrun_lost_samples(), (double)effective_sample_rate_hz(),
                (double)s_rec_clip_max_pct, s_rec_clip_flag ? 1 : 0);
        fclose(s_sidecar);
        s_sidecar = NULL;
    }
    if (s_file != NULL) {
        // Latch a final checkpoint at the exact end, then persist any pending ones.
        sync_add_checkpoint();
        fflush(s_file);
        sync_flush_checkpoints();
        fflush(s_file); // commit any buffered sample data before seeking back
        // Refresh the header's sample_rate_hz with the end-of-recording ODR. The
        // cumulative estimate is most accurate now (longest baseline), while the
        // header was written at start with whatever estimate existed then. Only
        // this one 4-byte field is rewritten -- magic/version are left untouched,
        // so a power cut mid-write can at worst garble the rate, never make the
        // file unreadable.
        float final_rate = effective_sample_rate_hz();
        if (fseek(s_file, offsetof(recording_header_t, sample_rate_hz), SEEK_SET) == 0) {
            fwrite(&final_rate, sizeof(final_rate), 1, s_file);
            fflush(s_file);
            fsync(fileno(s_file));
        }
        fclose(s_file);
        s_file = NULL;
    }
}

static void check_rotation(void) {
    if (s_bytes_written < ROTATION_THRESHOLD_BYTES) {
        return;
    }
    close_current_file("rotate");
    s_rotation_part++;
    if (!open_new_file(s_current_tag, s_rotation_part)) {
        // s_last_error already set by open_new_file(). The just-completed
        // part is already safely closed above; nothing left to do but stop
        // cleanly rather than spin with s_file == NULL forever.
        printf("recorder: rotation failed, stopping recording\n");
        s_state = REC_IDLE;
        xSemaphoreGive(s_stop_done_sem);
    }
}

// Smallest full-scale range (+-2/4/8/16g) whose limit is >= g.
static iis3dwb_fs_t pick_fs_for_g(float g) {
    if (g <= 2.0f) return IIS3DWB_FS_2G;
    if (g <= 4.0f) return IIS3DWB_FS_4G;
    if (g <= 8.0f) return IIS3DWB_FS_8G;
    return IIS3DWB_FS_16G;
}

// The next range up from `fs` by g value (the FS enum isn't monotonic).
static iis3dwb_fs_t next_fs_up(iis3dwb_fs_t fs) {
    float g = fs_to_g_value(fs);
    if (g < 4.0f) return IIS3DWB_FS_4G;
    if (g < 8.0f) return IIS3DWB_FS_8G;
    return IIS3DWB_FS_16G;
}

// Auto-range step: the current part clipped for a sustained period, so widen the
// range and continue in a new rotation part. Runs in the writer task between
// files (the caller has already closed the current part). Probes the true peak
// at +-16g for ~AUTORANGE_PROBE_MS -- keeping the rings drained so they don't
// overflow -- then opens the next part at the smallest range that fits the peak.
// `chunk`/`scratch` are the writer's own drain buffers, reused here.
static void auto_range_rotate(uint8_t *chunk, uint8_t *scratch) {
    iis3dwb_fs_t old_fs = accel_capture_get_fs();
    iis3dwb_filter_mode_t filter = accel_capture_get_filter_mode();
    iis3dwb_hpcf_t cutoff = accel_capture_get_hpcf();
    float old_g = fs_to_g_value(old_fs);

    // Widen to the max range so the true (unclipped) peak becomes measurable.
    // Transient: an auto-range change must never overwrite a remembered preference.
    // The filter (mode + cutoff) is carried through unchanged.
    accel_capture_set_config_transient(IIS3DWB_FS_16G, filter, cutoff);

    ring_buffer_t *full = accel_capture_get_recording_ring();
    ring_buffer_t *decim = accel_capture_get_recording_decim_ring();
    // Drop what was captured at the old range / during the switch.
    while (ring_buffer_read(full, chunk, WRITE_CHUNK_SIZE) > 0) {
    }
    while (ring_buffer_read(decim, scratch, WRITE_CHUNK_SIZE) > 0) {
    }

    // Measure the peak over the probe window, draining both rings so they don't
    // overflow while no file is open.
    int32_t max_abs = 0;
    int64_t end = esp_timer_get_time() + (int64_t)AUTORANGE_PROBE_MS * 1000;
    while (esp_timer_get_time() < end) {
        size_t n = ring_buffer_read(full, chunk, WRITE_CHUNK_SIZE);
        for (size_t i = 0; i + 1 < n; i += 2) {
            int16_t v = (int16_t)((uint16_t)chunk[i] | ((uint16_t)chunk[i + 1] << 8));
            int32_t a = (v < 0) ? -(int32_t)v : v;
            if (a > max_abs) {
                max_abs = a;
            }
        }
        ring_buffer_read(decim, scratch, WRITE_CHUNK_SIZE); // keep it drained too
        if (n == 0) {
            vTaskDelay(1); // nothing buffered yet -- yield rather than spin
        }
    }

    float sens_16 = iis3dwb_sensitivity_mg_per_lsb(IIS3DWB_FS_16G); // mg/LSB
    float peak_g = (float)max_abs * sens_16 / 1000.0f;
    iis3dwb_fs_t new_fs = pick_fs_for_g(peak_g * AUTORANGE_HEADROOM);
    if (fs_to_g_value(new_fs) <= old_g) {
        new_fs = next_fs_up(old_fs); // clipping was real -- always step up at least once
    }
    accel_capture_set_config_transient(new_fs, filter, cutoff);

    printf("recorder: auto-range: sustained clipping at +-%.0fg (peak ~%.2fg) -> +-%.0fg\n",
           (double)old_g, (double)peak_g, (double)fs_to_g_value(new_fs));

    s_rotation_part++;
    if (!open_new_file(s_current_tag, s_rotation_part)) {
        printf("recorder: auto-range: could not open next part -- stopping\n");
        s_state = REC_IDLE;
        xSemaphoreGive(s_stop_done_sem);
        return;
    }
    // Provenance for the new part: why it exists and the range it came from.
    if (s_sidecar != NULL) {
        fprintf(s_sidecar, "autorange reason=clipping from_fs_g=%.0f peak_g=%.2f\n", (double)old_g,
                (double)peak_g);
        fflush(s_sidecar);
    }
}

// Tracks the recording's clipping for the "clipped" integrity badge, regardless
// of the auto-range toggle. Cumulative across parts (reset per recording);
// throttled to ~1s windows. Sets the flag only on sustained clipping.
static void update_clip_flag(void) {
    if (s_state != REC_RECORDING) {
        return;
    }
    int64_t now = esp_timer_get_time();
    if (now - s_rec_clip_check_us < 1000000) {
        return;
    }
    s_rec_clip_check_us = now;
    uint32_t total, clip[3];
    accel_capture_get_clip_counts(&total, clip);
    uint32_t d_total = total - s_rec_clip_last_total;
    s_rec_clip_last_total = total;
    float max_pct = 0.0f;
    for (int i = 0; i < 3; i++) {
        uint32_t d = clip[i] - s_rec_clip_last[i];
        s_rec_clip_last[i] = clip[i];
        float pct = (d_total > 0) ? (100.0f * (float)d / (float)d_total) : 0.0f;
        if (pct > max_pct) {
            max_pct = pct;
        }
    }
    if (max_pct > s_rec_clip_max_pct) {
        s_rec_clip_max_pct = max_pct;
    }
    if (max_pct >= CLIP_FLAG_PCT) {
        if (++s_rec_clip_run >= CLIP_FLAG_SUSTAIN) {
            s_rec_clip_flag = true;
        }
    } else {
        s_rec_clip_run = 0;
    }
}

// Watches the recording's clipping while auto-range is enabled; on sustained
// full-scale saturation it steps the range up (auto_range_rotate). Called each
// writer loop with the drain buffers; throttles its own evaluation to ~1s
// windows and backs off for a cooldown after a change.
static void maybe_auto_range(uint8_t *chunk, uint8_t *scratch) {
    if (!s_auto_range || s_state != REC_RECORDING) {
        s_ar_high_since_us = 0;
        return;
    }
    int64_t now = esp_timer_get_time();
    if (now < s_ar_cooldown_until_us) {
        return;
    }
    if (fs_to_g_value(accel_capture_get_fs()) >= 16.0f) {
        return; // already at the widest range -- nothing to escalate to
    }
    if (now - s_ar_check_us < (int64_t)AUTORANGE_CHECK_MS * 1000) {
        return;
    }
    s_ar_check_us = now;

    uint32_t total, clip[3];
    accel_capture_get_clip_counts(&total, clip);
    uint32_t d_total = total - s_ar_last_total;
    s_ar_last_total = total;
    float max_pct = 0.0f;
    for (int i = 0; i < 3; i++) {
        uint32_t d = clip[i] - s_ar_last_clip[i];
        s_ar_last_clip[i] = clip[i];
        float pct = (d_total > 0) ? (100.0f * (float)d / (float)d_total) : 0.0f;
        if (pct > max_pct) {
            max_pct = pct;
        }
    }

    if (max_pct < AUTORANGE_TRIGGER_PCT) {
        s_ar_high_since_us = 0;
        return;
    }
    if (s_ar_high_since_us == 0) {
        s_ar_high_since_us = now;
    } else if (now - s_ar_high_since_us >= (int64_t)AUTORANGE_SUSTAIN_MS * 1000) {
        close_current_file("clip-rotate");
        auto_range_rotate(chunk, scratch);
        s_ar_high_since_us = 0;
        s_ar_cooldown_until_us = esp_timer_get_time() + (int64_t)AUTORANGE_COOLDOWN_MS * 1000;
    }
}

static void sd_writer_task(void *arg) {
    ring_buffer_t *full_ring = accel_capture_get_recording_ring();
    ring_buffer_t *decim_ring = accel_capture_get_recording_decim_ring();
    uint8_t *chunk = heap_caps_malloc(WRITE_CHUNK_SIZE, MALLOC_CAP_DEFAULT);
    // Only used to drain (and discard) whichever ring the current recording
    // *isn't* reading from -- same "always fed, always drained" contract
    // every ring in this project follows, so the idle one never backs up
    // and starts dropping while it waits to maybe be used next time.
    uint8_t *scratch = heap_caps_malloc(WRITE_CHUNK_SIZE, MALLOC_CAP_DEFAULT);

    for (;;) {
        if (s_start_requested) {
            s_start_requested = false;
            strncpy(s_current_tag, s_pending_tag, sizeof(s_current_tag) - 1);
            s_current_tag[sizeof(s_current_tag) - 1] = '\0';
            s_current_decimated = s_pending_decimated;
            s_rotation_part = 1;
            if (open_new_file(s_current_tag, s_rotation_part)) {
                // Fresh recording: reset markers and capture the start reference
                // (used for every marker's elapsed-time position, across parts).
                s_recording_start_us = esp_timer_get_time();
                if (s_marker_mutex != NULL) {
                    xSemaphoreTake(s_marker_mutex, portMAX_DELAY);
                    s_marker_count = 0;
                    s_next_marker_id = 1;
                    snprintf(s_marker_path, sizeof(s_marker_path), "%s", s_current_filename);
                    size_t mlen = strlen(s_marker_path);
                    if (mlen > 4 && strcmp(s_marker_path + mlen - 4, ".bin") == 0) {
                        strcpy(s_marker_path + mlen - 4, ".mark");
                    } else {
                        s_marker_path[0] = '\0';
                    }
                    xSemaphoreGive(s_marker_mutex);
                }
                s_state = REC_RECORDING;
                // Reset the auto-range clip monitor for this fresh recording.
                accel_capture_get_clip_counts(&s_ar_last_total, s_ar_last_clip);
                s_ar_check_us = esp_timer_get_time();
                s_ar_high_since_us = 0;
                s_ar_cooldown_until_us = 0;
                // Reset the recording-level clip flag/stat for this recording.
                s_rec_clip_max_pct = 0.0f;
                s_rec_clip_run = 0;
                s_rec_clip_flag = false;
                s_rec_clip_check_us = esp_timer_get_time();
                s_rec_clip_last_total = s_ar_last_total;
                s_rec_clip_last[0] = s_ar_last_clip[0];
                s_rec_clip_last[1] = s_ar_last_clip[1];
                s_rec_clip_last[2] = s_ar_last_clip[2];
            } else {
                s_state = REC_IDLE;
            }
        }

        ring_buffer_t *active_ring = s_current_decimated ? decim_ring : full_ring;
        ring_buffer_t *idle_ring = s_current_decimated ? full_ring : decim_ring;
        size_t idle_n = ring_buffer_read(idle_ring, scratch, WRITE_CHUNK_SIZE);

        size_t n = ring_buffer_read(active_ring, chunk, WRITE_CHUNK_SIZE);
        if (n > 0 && (s_state == REC_RECORDING || s_state == REC_STOP_REQUESTED) &&
            s_file != NULL) {
            size_t written = fwrite(chunk, 1, n, s_file);
            int write_errno = errno; // capture before close_current_file perturbs it
            s_bytes_written += written;
            if (written < n) {
                // A short write stops the recording either way -- never keep
                // dribbling data into a broken file. But a full card (ENOSPC) is
                // an expected, graceful end (the user filled the card on purpose),
                // not a failure: close cleanly, keep everything written, and show
                // a friendly notice rather than a red error. Any other errno is a
                // real fault and stays an error.
                if (write_errno == ENOSPC) {
                    snprintf(s_last_notice, sizeof(s_last_notice),
                             "SD card full - recording stopped and saved");
                    s_last_error[0] = '\0';
                    printf("recorder: SD card full -- recording stopped gracefully\n");
                    close_current_file("full");
                } else {
                    snprintf(s_last_error, sizeof(s_last_error), "write failed (errno=%d %s)",
                             write_errno, strerror(write_errno));
                    s_last_error_at_us = esp_timer_get_time();
                    printf("recorder: %s -- stopping recording\n", s_last_error);
                    close_current_file("error");
                }
                s_state = REC_IDLE;
                xSemaphoreGive(s_stop_done_sem);
            } else {
                check_rotation();
                maybe_auto_range(chunk, scratch);
                update_clip_flag();
            }
        }
        // else: idle (not recording) -- discard, or nothing available right now

        // Periodic sync checkpoint: pair the current sample count with the shared
        // TSF every ~60 s so recordings from multiple devices can be time-aligned.
        // Latched here (RAM); persisted by the next sync_file() below.
        if (s_state == REC_RECORDING && s_file != NULL &&
            esp_timer_get_time() - s_sync_last_us >= (int64_t)SYNC_CHECKPOINT_INTERVAL_S * 1000000) {
            sync_add_checkpoint();
        }

        // 1 Hz aux row (temperatures, power, signal, loss counters) -- see aux_write_row.
        if (s_state == REC_RECORDING && s_aux != NULL &&
            esp_timer_get_time() - s_aux_last_us >= AUX_INTERVAL_US) {
            aux_write_row();
        }

        // Periodic durability sync (see FSYNC_INTERVAL_MS): commit the file's
        // metadata so a power loss / yanked card costs at most this interval,
        // not the whole in-progress file. Only while actively recording; a
        // pending stop closes (and thus syncs) the file momentarily anyway.
        if (s_state == REC_RECORDING && s_file != NULL &&
            esp_timer_get_time() - s_last_fsync_us >= (int64_t)FSYNC_INTERVAL_MS * 1000) {
            sync_file();
        }

        if (s_state == REC_STOP_REQUESTED && n == 0) {
            close_current_file("stop");
            s_state = REC_IDLE;
            xSemaphoreGive(s_stop_done_sem);
        }

        if (n == 0 && idle_n == 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}

// Self-description of the recording folder for whoever (or whatever -- typically an
// analysis script or AI agent) gets handed the card without this repo. Compiled in,
// so it always matches the running firmware; README.md has the long form. Keep it
// in sync with recording_format.h, the .log fprintf()s, and aux_open() above.
static const char s_format_doc[] =
    "# VibraShock recording files\n"
    "\n"
    "Generated by VibraShock firmware " FIRMWARE_VERSION "; rewritten automatically when outdated, do not edit.\n"
    "Binary values little-endian. Times use the device clock unless named unix (UTC epoch seconds).\n"
    "\n"
    "## Files (same base name)\n"
    "- `<base>.bin`: header + samples (below). Rotates at ~3.5 GB or on an auto-range change into\n"
    "  `<base>_part2.bin`, `_part3`, ...; each part is standalone and its sample_index restarts at 0.\n"
    "- `<base>.log`: text events per part, one line each: `<event> key=value ...`\n"
    "- `<base>.aux.csv`: 1 Hz side channels per part (header row names the columns).\n"
    "- `<base>.mark`: user markers for the whole recording (one file, part-1 base name).\n"
    "- `<base>.csv`: optional export of a .bin: `sample_index,time_s,unix_time_s,x_g,y_g,z_g`.\n"
    "\n"
    "## .bin\n"
    "64-byte packed header:\n"
    "\n"
    "| off | type | field |\n"
    "|---|---|---|\n"
    "| 0 | char[4] | magic `VREC` |\n"
    "| 4 | u16 | version: 3 (2 = no sync table) |\n"
    "| 8 | f32 | sample_rate_hz: measured rate (decimated recordings: exact reduced rate) |\n"
    "| 12 | f32 | fs_range_g: +-2/4/8/16 g |\n"
    "| 16 | f32 | sensitivity_mg_per_lsb |\n"
    "| 20 | u8 | wall_time_valid: 1 if wall_time_unix_s is real |\n"
    "| 24 | i64 | boot_time_us: device uptime at start |\n"
    "| 32 | i64 | wall_time_unix_s: start time |\n"
    "| 40 | u32 | sync_table_bytes (0 in v2) |\n"
    "| 44 | u32 | sync_count: valid sync entries |\n"
    "| 48 | u8[6] | bssid: Wi-Fi AP at start (zeros = none) |\n"
    "| 54 | u16 | sync_interval_s |\n"
    "\n"
    "Other header bytes are reserved.\n"
    "\n"
    "Sync table at offset 64, sync_table_bytes long; the first sync_count 24-byte entries are valid:\n"
    "`u64 sample_index, i64 tsf_us, u8 bssid[6], u8 pad[2]`. tsf_us is the 802.11 TSF, a us clock\n"
    "shared by all devices on the same AP (bssid); 0 = not connected. Taken at start, every ~60 s and\n"
    "at the end. To align devices, pair entries with equal bssid.\n"
    "\n"
    "Samples from offset 64 + sync_table_bytes to EOF: int16 x, y, z per sample (6 bytes).\n"
    "count = (file_size - 64 - sync_table_bytes) / 6; g = raw * sensitivity_mg_per_lsb / 1000;\n"
    "t = index / sample_rate_hz. No per-sample timestamps; lost samples leave no gap marker\n"
    "(see lost_samples/drops).\n"
    "\n"
    "## .log\n"
    "`start` opens a part; it ends with exactly one of `stop`, `rotate` (size limit), `clip-rotate`\n"
    "(auto-range), `full` (card full), `error` (write error). No end line = power loss or crash;\n"
    "data up to the last ~5 s sync is valid. `autorange` after `start` = part created by auto-range.\n"
    "Keys: part, dev (board id), fw, fs_g, rate_hz (estimate at that moment; header has the final\n"
    "value), lpf2 (legacy on/off), filter (off|lowpass|highpass), filter_hz (cutoff), temp_c (sensor\n"
    "die), bytes (file size incl. header), dur_s. overruns, drops, lost_samples are cumulative since\n"
    "boot: end minus start = this part; all should be 0. clip_pct: highest 1 s share of full-scale\n"
    "samples on any axis (%), recording so far. clipped=1: >= 0.3 % for 3 s in a row.\n"
    "\n"
    "## .aux.csv\n"
    "One row per second, plus the first and last moment of the part. Empty field = unavailable.\n"
    "sample_index: position in this part's .bin. elapsed_s: since part start. accel_temp_c: sensor\n"
    "die; mcu_temp_c: ESP32 die; charger_temp_c: charger die. ibus_ma: USB input current. ibat_ma:\n"
    "battery current, + charging / - discharging. battery_pct: estimate. charging: 0/1.\n"
    "lost_samples, drops: since part start, should stay 0. clipped_x/y/z: full-scale samples since\n"
    "the previous row, counted at the full 26.7 kHz.\n"
    "\n"
    "## .mark\n"
    "CSV `id,time_s,unix_time_s,note`. time_s: seconds since the recording started (part 1), device\n"
    "clock, continuing across parts. unix_time_s empty if the clock was not synced.\n";

// Writes <rec>/FORMAT.md if it's missing or differs from the compiled-in text (so it
// tracks firmware updates without rewriting the card on every boot). Best-effort.
static void write_format_doc(void) {
    const char *path = REC_DIR "/FORMAT.md";
    const size_t len = sizeof(s_format_doc) - 1;
    bool same = false;
    FILE *f = fopen(path, "rb");
    if (f != NULL) {
        char *cur = malloc(len + 1);
        if (cur != NULL) {
            // Equal only if exactly `len` bytes match and nothing follows.
            same = fread(cur, 1, len + 1, f) == len && memcmp(cur, s_format_doc, len) == 0;
            free(cur);
        }
        fclose(f);
    }
    if (same) {
        return;
    }
    f = fopen(path, "wb");
    if (f == NULL) {
        printf("recorder: could not write %s (errno=%d)\n", path, errno);
        return;
    }
    fwrite(s_format_doc, 1, len, f);
    fclose(f);
}

// Does the actual mount attempt (used both at boot and on every remount).
// Safe to call when nothing is inserted -- it just fails/times out and
// leaves s_sd_mounted false, which is the expected "no card" state.
static bool mount_sd_card(void) {
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_1;
    // Bump from the conservative 20MHz "default speed" to 40MHz "high
    // speed" -- most SD cards support it, and it roughly doubles the raw
    // bus bandwidth ceiling.
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.clk = PIN_SD_CLK;
    slot_config.cmd = PIN_SD_CMD;
    slot_config.d0 = PIN_SD_DAT0;
    slot_config.d1 = PIN_SD_DAT1;
    slot_config.d2 = PIN_SD_DAT2;
    slot_config.d3 = PIN_SD_DAT3;
    // Card-detect is handled ourselves (see sd_cd_monitor_task) rather than
    // handed to the SDMMC peripheral, since we don't know this socket's CD
    // polarity and a wrong assumption there would make the driver refuse to
    // even attempt a mount.
    slot_config.cd = SDMMC_SLOT_NO_CD;
    slot_config.wp = SDMMC_SLOT_NO_WP;
    slot_config.width = 4;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 8, // recording .bin/.log/.aux.csv + marker save + downloads; 4 KB each, PSRAM
        .allocation_unit_size = WRITE_CHUNK_SIZE,
    };

    esp_err_t err =
        esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &s_card);
    if (err != ESP_OK) {
        printf("recorder: SD mount failed: %s\n", esp_err_to_name(err));
        s_sd_mounted = false;
        return false;
    }
    s_sd_mounted = true;
    // Diagnostic: dump the negotiated bus parameters (speed + width) to the serial
    // log. Slow benchmarks with no bus errors come down to what actually got
    // negotiated -- e.g. "Speed: 40 MHz" / "Bus width: 4" means the bus is fine and
    // the card is the limiter, whereas a fallback (20 MHz, or 1-bit) points at
    // host-side timing instead. Cheap to print once per mount.
    if (s_card != NULL) {
        printf("recorder: SD mounted -- negotiated bus parameters:\n");
        sdmmc_card_print_info(stdout, s_card);
    }
    // The card is good now, so drop any stale storage error (e.g. a
    // "SD card not mounted" from a start attempt while the card was out) --
    // otherwise the dashboard keeps showing it after a re-insert/remount.
    s_last_error[0] = '\0';

    if (mkdir(REC_DIR, 0755) != 0 && errno != EEXIST) {
        printf("recorder: mkdir %s failed (errno=%d %s)\n", REC_DIR, errno, strerror(errno));
    }
    write_format_doc();
    return true;
}

static void sd_cd_monitor_task(void *arg) {
    gpio_config_t cd_conf = {
        .pin_bit_mask = 1ULL << PIN_SD_CD,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cd_conf);

    int last_raw = gpio_get_level(PIN_SD_CD);
    int stable_raw = last_raw;
    int stable_count = 0;

    for (;;) {
        int raw = gpio_get_level(PIN_SD_CD);
        if (raw == last_raw) {
            stable_count++;
        } else {
            stable_count = 0;
            last_raw = raw;
        }

        if (stable_count == CD_DEBOUNCE_POLLS && raw != stable_raw) {
            stable_raw = raw;
            printf("recorder: card-detect level changed (raw=%d) -- remounting\n", raw);
            recorder_remount();
        }

        vTaskDelay(pdMS_TO_TICKS(CD_POLL_INTERVAL_MS));
    }
}

bool recorder_init(void) {
    s_stop_done_sem = xSemaphoreCreateBinary();
    if (s_marker_mutex == NULL) {
        s_marker_mutex = xSemaphoreCreateMutex();
    }
    nvs_handle_t nh;
    if (nvs_open(AUTORANGE_NVS_NS, NVS_READONLY, &nh) == ESP_OK) {
        uint8_t v = 0;
        if (nvs_get_u8(nh, "auto_range", &v) == ESP_OK) {
            s_auto_range = (v != 0);
        }
        nvs_close(nh);
    }
    mount_sd_card(); // failure is fine -- just means "no card yet"
    xTaskCreate(sd_cd_monitor_task, "sd_cd_monitor", 3072, NULL, 4, NULL);
    return s_sd_mounted;
}

bool recorder_get_auto_range(void) {
    return s_auto_range;
}

void recorder_set_auto_range(bool enable) {
    s_auto_range = enable;
    nvs_handle_t h;
    if (nvs_open(AUTORANGE_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "auto_range", enable ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

bool recorder_is_sd_mounted(void) {
    return s_sd_mounted;
}

bool recorder_get_sd_space(uint64_t *total_bytes, uint64_t *free_bytes) {
    // Throttled: the free-cluster count changes slowly, so re-query the card at
    // most every few seconds even if polled often. Single caller (the status
    // handler task), so plain statics are fine.
    static uint64_t cached_total = 0, cached_free = 0;
    static int64_t last_query_us = 0;
    static bool valid = false;

    if (!s_sd_mounted) {
        valid = false;
        return false;
    }
    int64_t now = esp_timer_get_time();
    if (!valid || now - last_query_us > 3000000) {
        uint64_t t = 0, f = 0;
        if (esp_vfs_fat_info(MOUNT_POINT, &t, &f) != ESP_OK) {
            return false;
        }
        cached_total = t;
        cached_free = f;
        last_query_us = now;
        valid = true;
    }
    if (total_bytes) {
        *total_bytes = cached_total;
    }
    if (free_bytes) {
        *free_bytes = cached_free;
    }
    return true;
}

bool recorder_remount(void) {
    if (s_state == REC_RECORDING || s_state == REC_STOP_REQUESTED) {
        recorder_request_stop();
        recorder_wait_for_stop(3000);
    }
    if (s_sd_mounted) {
        esp_vfs_fat_sdcard_unmount(MOUNT_POINT, s_card);
        s_sd_mounted = false;
        s_card = NULL;
    }
    return mount_sd_card();
}

// Rewrites the whole marker sidecar from the in-RAM list (caller holds
// s_marker_mutex). Rewriting a tiny file on every change keeps it always
// current on disk and makes edit/delete trivial; no marks -> no sidecar.
static void write_marker_sidecar(void) {
    if (s_marker_path[0] == '\0') {
        return;
    }
    if (s_marker_count == 0) {
        remove(s_marker_path);
        return;
    }
    FILE *mf = fopen(s_marker_path, "w");
    if (mf == NULL) {
        return;
    }
    // CSV-structured (comma-separated, matching the recording data export, with
    // a header once) so the markers can be imported as events into external
    // analysis tools. The note is quoted so a comma in it can't break the row;
    // `unix_time_s` is blank when the device clock wasn't synced at record time.
    fprintf(mf, "id,time_s,unix_time_s,note\n");
    for (int i = 0; i < s_marker_count; i++) {
        char unix_col[24] = "";
        if (s_markers[i].wall_unix_s > 0) {
            snprintf(unix_col, sizeof(unix_col), "%lld", (long long)s_markers[i].wall_unix_s);
        }
        fprintf(mf, "%u,%.3f,%s,\"%s\"\n", (unsigned)s_markers[i].id, s_markers[i].elapsed_s,
                unix_col, s_markers[i].note);
    }
    fflush(mf);
    fsync(fileno(mf));
    fclose(mf);
}

// Strips characters that would break the CSV sidecar's quoted note field
// (quotes, backslashes, control chars) and length-caps. Commas are fine -- the
// note is written quoted.
static void sanitize_note(const char *in, char *out, size_t out_len) {
    size_t j = 0;
    for (size_t i = 0; in != NULL && in[i] != '\0' && j < out_len - 1; i++) {
        char c = in[i];
        if (c == '"' || c == '\\' || (unsigned char)c < 0x20) {
            c = ' ';
        }
        out[j++] = c;
    }
    out[j] = '\0';
}

int recorder_marker_add(int64_t at_us) {
    if (s_state != REC_RECORDING || s_marker_mutex == NULL) {
        return -1;
    }
    xSemaphoreTake(s_marker_mutex, portMAX_DELAY);
    int result = -1;
    if (s_marker_count < MAX_MARKERS) {
        int64_t t = at_us > 0 ? at_us : esp_timer_get_time();
        double elapsed =
            s_recording_start_us > 0 ? (double)(t - s_recording_start_us) / 1000000.0 : 0.0;
        if (elapsed < 0) {
            elapsed = 0;
        }
        struct tm tm_info;
        // Back-date the wall clock to the mark moment (button marks resolve a
        // few hundred ms after the press); elapsed_s stays the precise position.
        int64_t wall = 0;
        if (wallclock_is_valid(&tm_info)) {
            wall = (int64_t)time(NULL) - (int64_t)((esp_timer_get_time() - t) / 1000000);
        }
        recorder_marker_t *m = &s_markers[s_marker_count++];
        m->id = s_next_marker_id++;
        m->elapsed_s = elapsed;
        m->wall_unix_s = wall;
        m->note[0] = '\0';
        write_marker_sidecar();
        result = (int)m->id;
    }
    xSemaphoreGive(s_marker_mutex);
    return result;
}

bool recorder_marker_set_note(uint32_t id, const char *note) {
    if (s_marker_mutex == NULL) {
        return false;
    }
    xSemaphoreTake(s_marker_mutex, portMAX_DELAY);
    bool found = false;
    for (int i = 0; i < s_marker_count; i++) {
        if (s_markers[i].id == id) {
            sanitize_note(note, s_markers[i].note, sizeof(s_markers[i].note));
            write_marker_sidecar();
            found = true;
            break;
        }
    }
    xSemaphoreGive(s_marker_mutex);
    return found;
}

bool recorder_marker_delete(uint32_t id) {
    if (s_marker_mutex == NULL) {
        return false;
    }
    xSemaphoreTake(s_marker_mutex, portMAX_DELAY);
    bool found = false;
    for (int i = 0; i < s_marker_count; i++) {
        if (s_markers[i].id == id) {
            for (int j = i; j < s_marker_count - 1; j++) {
                s_markers[j] = s_markers[j + 1];
            }
            s_marker_count--;
            write_marker_sidecar();
            found = true;
            break;
        }
    }
    xSemaphoreGive(s_marker_mutex);
    return found;
}

int recorder_marker_list(recorder_marker_t *out, int max) {
    if (s_marker_mutex == NULL || out == NULL || max <= 0) {
        return 0;
    }
    xSemaphoreTake(s_marker_mutex, portMAX_DELAY);
    int n = s_marker_count < max ? s_marker_count : max;
    for (int i = 0; i < n; i++) {
        out[i] = s_markers[i];
    }
    xSemaphoreGive(s_marker_mutex);
    return n;
}

int recorder_marker_count(void) {
    return s_marker_count; // plain read; a slightly stale count for status is fine
}

bool recorder_start_writer_task(void) {
    BaseType_t created =
        xTaskCreatePinnedToCore(sd_writer_task, "sd_writer", 8192, NULL, 12, NULL, 1);
    return created == pdPASS;
}

bool recorder_request_start(const char *name_tag, bool decimated) {
    if (s_state != REC_IDLE) {
        snprintf(s_last_error, sizeof(s_last_error), "already recording");
        s_last_error_at_us = esp_timer_get_time();
        return false;
    }
    if (!s_sd_mounted) {
        snprintf(s_last_error, sizeof(s_last_error), "SD card not mounted");
        s_last_error_at_us = esp_timer_get_time();
        return false;
    }
    s_last_error[0] = '\0'; // this attempt is starting fresh
    s_last_notice[0] = '\0';
    strncpy(s_pending_tag, name_tag != NULL ? name_tag : "", sizeof(s_pending_tag) - 1);
    s_pending_tag[sizeof(s_pending_tag) - 1] = '\0';
    s_pending_decimated = decimated;
    s_start_requested = true;
    return true;
}

void recorder_request_stop(void) {
    if (s_state == REC_RECORDING) {
        // Drain a stale give left by a caller that requested a stop but
        // never consumed the confirmation (e.g. the dashboard's
        // fire-and-forget stop button) -- otherwise the NEXT call to
        // recorder_wait_for_stop() would get a false-positive immediate
        // true from THIS stop's leftover give, not the one it actually
        // just requested.
        xSemaphoreTake(s_stop_done_sem, 0);
        s_state = REC_STOP_REQUESTED;
    }
}

bool recorder_wait_for_stop(uint32_t timeout_ms) {
    if (s_state == REC_IDLE) {
        return true;
    }
    return xSemaphoreTake(s_stop_done_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

bool recorder_is_recording(void) {
    return s_state != REC_IDLE;
}

const char *recorder_get_current_filename(void) {
    return (s_state != REC_IDLE) ? s_current_filename : "";
}

uint64_t recorder_get_bytes_written(void) {
    return s_bytes_written;
}

float recorder_get_current_sample_rate_hz(void) {
    return s_current_decimated ? accel_capture_get_recording_decim_rate_hz() : 26700.0f;
}

const char *recorder_get_last_error(void) {
    return s_last_error;
}

const char *recorder_get_last_notice(void) {
    return s_last_notice;
}

int64_t recorder_last_error_at_us(void) {
    return s_last_error_at_us;
}
