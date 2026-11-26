#include "recorder.h"

#include <dirent.h>
#include <errno.h>
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
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "pins.h"
#include "ring_buffer.h"
#include "scan_log.h"
#include "sdmmc_cmd.h"
#include "version.h"

#define MOUNT_POINT "/sdcard"

#define WRITE_CHUNK_SIZE (32 * 1024)
// Rotate well below FAT32's 4 GB limit; 1 GB CSVs are still easy to open.
#define ROTATION_THRESHOLD_BYTES (1024ULL * 1024 * 1024)
// Line buffer between the producers (scan_log rows) and the SD writer. 1 MB is
// ~8 s at a very busy 125 KB/s -- far longer than an SD card's worst stall.
#define LINE_RING_BYTES (1024 * 1024)

// Periodic durability sync: FATFS commits a file's size/FAT chain only on
// f_sync/f_close, so this bounds what a power loss or yanked card can cost.
#define FSYNC_INTERVAL_MS 5000

// Card-detect debounce (mechanical switches bounce during insertion).
#define CD_POLL_INTERVAL_MS 200
#define CD_DEBOUNCE_POLLS 5 // ~1 s stable

#define MAX_MARKERS 64

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
static int s_part = 1;
// Shared file-name base of the session ("<ts>_<tag>" or "log_<seq>_<tag>"),
// computed once; every rotation part reuses it with "_partN" appended.
static char s_base[100] = "";

static FILE *s_file = NULL;
static FILE *s_sidecar = NULL; // <base>.log
static char s_current_filename[176] = "";
static volatile uint64_t s_bytes_written = 0; // whole session
static volatile uint64_t s_part_bytes = 0;
static volatile uint64_t s_rows_written = 0;
static int64_t s_session_start_us = 0;
static int64_t s_session_end_us = 0;
static int64_t s_last_fsync_us = 0;

// Line buffer. Producers (several tasks) take s_line_mux; the writer task is
// the single consumer. s_accept gates which lines are taken: true only while
// a session is open, so nothing is buffered when idle.
static ring_buffer_t s_lines;
static bool s_lines_ok = false;
static SemaphoreHandle_t s_line_mux = NULL; // recursive: session callbacks log while it's held
static volatile bool s_accept = false;
static volatile uint32_t s_log_drops = 0;

// Write-rate estimate (bytes/s over ~10 s), for the "SD time left" readouts.
static float s_rate_bps = 0.0f;
static int64_t s_rate_t0 = 0;
static uint64_t s_rate_b0 = 0;

static char s_last_error[64] = "";
static int64_t s_last_error_at_us = 0;
static char s_last_notice[64] = "";

static recorder_marker_t s_markers[MAX_MARKERS];
static int s_marker_count = 0;
static uint32_t s_next_marker_id = 1;
static SemaphoreHandle_t s_marker_mutex = NULL;
static char s_marker_path[176] = "";

static SemaphoreHandle_t s_stop_done_sem;

static void set_error(const char *msg) {
    snprintf(s_last_error, sizeof(s_last_error), "%s", msg);
    s_last_error_at_us = esp_timer_get_time();
}

// --- File naming -------------------------------------------------------------

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
            // else: drop characters FAT32 file names can't hold safely
        }
    }
    out[oi] = '\0';
}

static bool wallclock_is_valid(struct tm *out_tm) {
    time_t now = time(NULL);
    gmtime_r(&now, out_tm);
    return (out_tm->tm_year + 1900) >= 2020; // before NTP/browser sync the clock reads ~1970
}

static uint32_t next_sequence_number(void) {
    uint32_t max_seq = 0;
    DIR *d = opendir(RECORDER_DIR);
    if (d != NULL) {
        struct dirent *entry;
        while ((entry = readdir(d)) != NULL) {
            unsigned int seq;
            if (sscanf(entry->d_name, "log_%u", &seq) == 1 && seq > max_seq) {
                max_seq = seq;
            }
        }
        closedir(d);
    }
    return max_seq + 1;
}

static void compute_base(const char *tag) {
    char safe_tag[64];
    sanitize_tag(tag, safe_tag, sizeof(safe_tag));
    struct tm tm_info;
    char head[32];
    if (wallclock_is_valid(&tm_info)) {
        strftime(head, sizeof(head), "%Y-%m-%d_%H%M%S", &tm_info); // UTC
    } else {
        snprintf(head, sizeof(head), "log_%05u", (unsigned)next_sequence_number());
    }
    if (safe_tag[0] != '\0') {
        snprintf(s_base, sizeof(s_base), "%s_%s", head, safe_tag);
    } else {
        snprintf(s_base, sizeof(s_base), "%s", head);
    }
}

static void part_path(int part, char *out, size_t out_len) {
    if (part <= 1) {
        snprintf(out, out_len, RECORDER_DIR "/%s.csv", s_base);
    } else {
        snprintf(out, out_len, RECORDER_DIR "/%s_part%d.csv", s_base, part);
    }
}

// --- Sidecar (.log) ------------------------------------------------------------

static void sidecar_line(const char *event) {
    if (s_sidecar == NULL) {
        return;
    }
    char devid[16];
    device_id_str(devid, sizeof(devid));
    struct tm tm_info;
    char utc[32] = "";
    if (wallclock_is_valid(&tm_info)) {
        strftime(utc, sizeof(utc), "%Y-%m-%dT%H:%M:%SZ", &tm_info);
    }
    double dur = s_session_start_us > 0 ? (double)(esp_timer_get_time() - s_session_start_us) / 1e6 : 0;
    fprintf(s_sidecar, "%s part=%d dev=%s fw=%s utc=%s uptime_ms=%lld bytes=%llu rows=%llu dur_s=%.1f log_drops=%u\n",
            event, s_part, devid, FIRMWARE_VERSION, utc[0] ? utc : "-",
            (long long)(esp_timer_get_time() / 1000), (unsigned long long)s_bytes_written,
            (unsigned long long)s_rows_written, dur, (unsigned)s_log_drops);
    fflush(s_sidecar);
    fsync(fileno(s_sidecar)); // commit now, so a power cut keeps the line
}

// --- Part files --------------------------------------------------------------

static bool open_part(int part) {
    char path[176];
    part_path(part, path, sizeof(path));
    s_file = fopen(path, "wb");
    if (s_file == NULL) {
        char msg[64];
        snprintf(msg, sizeof(msg), "open failed (errno=%d %s)", errno, strerror(errno));
        set_error(msg);
        printf("recorder: failed to open %s (errno=%d)\n", path, errno);
        return false;
    }
    snprintf(s_current_filename, sizeof(s_current_filename), "%s", path);
    s_part = part;
    s_part_bytes = 0;
    // Every part starts with the column header, so each one opens on its own.
    const char *hdr = scan_log_csv_header();
    size_t hl = strlen(hdr);
    fwrite(hdr, 1, hl, s_file);
    s_part_bytes += hl;
    s_bytes_written += hl;
    s_last_fsync_us = esp_timer_get_time();
    return true;
}

static void close_part(void) {
    if (s_file != NULL) {
        fflush(s_file);
        fsync(fileno(s_file));
        fclose(s_file);
        s_file = NULL;
    }
}

static void sync_files(void) {
    if (s_file != NULL) {
        fflush(s_file);
        fsync(fileno(s_file));
    }
    s_last_fsync_us = esp_timer_get_time();
}

// Writes `n` bytes of complete-or-partial lines. Rotation only ever happens at
// a line boundary, so no row is split across two parts.
static bool write_data(const uint8_t *buf, size_t n, int *err_out) {
    size_t off = 0;
    while (off < n) {
        size_t len = n - off;
        bool rotate = false;
        if (s_part_bytes + len >= ROTATION_THRESHOLD_BYTES) {
            // Write up to (and including) the last newline in this piece, then rotate.
            size_t k = len;
            while (k > 0 && buf[off + k - 1] != '\n') {
                k--;
            }
            if (k > 0) {
                len = k;
                rotate = true;
            }
        }
        size_t w = fwrite(buf + off, 1, len, s_file);
        s_bytes_written += w;
        s_part_bytes += w;
        if (w < len) {
            *err_out = errno;
            return false;
        }
        for (size_t i = 0; i < len; i++) {
            s_rows_written += (buf[off + i] == '\n');
        }
        off += len;
        if (rotate) {
            sidecar_line("rotate");
            close_part();
            if (!open_part(s_part + 1)) {
                *err_out = errno;
                return false;
            }
            sidecar_line("start");
        }
    }
    return true;
}

// --- Session lifecycle (writer task) ---------------------------------------------

static void session_open(void) {
    compute_base(s_pending_tag);
    s_bytes_written = 0;
    s_rows_written = 0;
    s_log_drops = 0;
    if (!open_part(1)) {
        s_state = REC_IDLE;
        return;
    }
    char path[176];
    snprintf(path, sizeof(path), RECORDER_DIR "/%s.log", s_base);
    s_sidecar = fopen(path, "a");
    s_session_start_us = esp_timer_get_time();
    s_session_end_us = 0;
    s_rate_t0 = s_session_start_us;
    s_rate_b0 = 0;
    s_rate_bps = 0;

    xSemaphoreTake(s_marker_mutex, portMAX_DELAY);
    s_marker_count = 0;
    s_next_marker_id = 1;
    snprintf(s_marker_path, sizeof(s_marker_path), RECORDER_DIR "/%s.mark", s_base);
    xSemaphoreGive(s_marker_mutex);

    sidecar_line("start");
    // Accept lines from here on; the START row goes in first, under the line
    // lock, so no scan row can get ahead of it.
    xSemaphoreTakeRecursive(s_line_mux, portMAX_DELAY);
    s_accept = true;
    scan_log_session_begin(s_base);
    xSemaphoreGiveRecursive(s_line_mux);
    s_state = REC_RECORDING;
    printf("recorder: logging to %s\n", s_current_filename);
}

static void session_close(const char *reason, uint8_t *chunk) {
    xSemaphoreTakeRecursive(s_line_mux, portMAX_DELAY);
    scan_log_session_end(reason);
    s_accept = false; // nothing more after the STOP row
    xSemaphoreGiveRecursive(s_line_mux);
    // Flush what's still buffered (best-effort if the card is the problem).
    if (s_file != NULL) {
        size_t n;
        int err = 0;
        while ((n = ring_buffer_read(&s_lines, chunk, WRITE_CHUNK_SIZE)) > 0) {
            if (!write_data(chunk, n, &err)) {
                break;
            }
        }
    }
    s_session_end_us = esp_timer_get_time();
    sidecar_line(reason);
    if (s_sidecar != NULL) {
        fclose(s_sidecar);
        s_sidecar = NULL;
    }
    close_part();
    s_state = REC_IDLE;
    xSemaphoreGive(s_stop_done_sem);
    printf("recorder: session ended (%s), %llu rows\n", reason, (unsigned long long)s_rows_written);
}

static void sd_writer_task(void *arg) {
    (void)arg;
    uint8_t *chunk = heap_caps_malloc(WRITE_CHUNK_SIZE, MALLOC_CAP_DEFAULT);
    uint8_t *scratch = heap_caps_malloc(4096, MALLOC_CAP_DEFAULT);
    // The accelerometer still fills its recording rings until the motion
    // pipeline replaces them (rework step 4). Keep draining them so capture
    // never backs up -- the data itself isn't recorded any more.
    ring_buffer_t *accel_full = accel_capture_get_recording_ring();
    ring_buffer_t *accel_decim = accel_capture_get_recording_decim_ring();

    for (;;) {
        if (accel_full != NULL) {
            while (ring_buffer_read(accel_full, scratch, 4096) > 0) {
            }
        }
        if (accel_decim != NULL) {
            while (ring_buffer_read(accel_decim, scratch, 4096) > 0) {
            }
        }

        if (s_start_requested) {
            s_start_requested = false;
            session_open();
        }

        size_t n = 0;
        if (s_state != REC_IDLE && s_file != NULL) {
            n = ring_buffer_read(&s_lines, chunk, WRITE_CHUNK_SIZE);
            if (n > 0) {
                int err = 0;
                if (!write_data(chunk, n, &err)) {
                    xSemaphoreTakeRecursive(s_line_mux, portMAX_DELAY);
                    s_accept = false;
                    xSemaphoreGiveRecursive(s_line_mux);
                    if (err == ENOSPC) {
                        snprintf(s_last_notice, sizeof(s_last_notice), "SD card full - logging stopped and saved");
                        printf("recorder: SD card full -- session stopped\n");
                        session_close("full", chunk);
                    } else {
                        char msg[64];
                        snprintf(msg, sizeof(msg), "write failed (errno=%d %s)", err, strerror(err));
                        set_error(msg);
                        printf("recorder: %s -- stopping\n", msg);
                        session_close("error", chunk);
                    }
                    continue;
                }
            }
            int64_t now = esp_timer_get_time();
            if (now - s_rate_t0 >= 10000000) {
                s_rate_bps = (float)(s_bytes_written - s_rate_b0) * 1e6f / (float)(now - s_rate_t0);
                s_rate_t0 = now;
                s_rate_b0 = s_bytes_written;
            }
            if (now - s_last_fsync_us >= (int64_t)FSYNC_INTERVAL_MS * 1000) {
                sync_files();
            }
            if (s_state == REC_STOP_REQUESTED) {
                session_close("stop", chunk);
                continue;
            }
        } else if (s_state == REC_STOP_REQUESTED) {
            session_close("stop", chunk);
        }

        if (n == 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}

bool recorder_log_line(const char *line, size_t len) {
    if (!s_lines_ok || !s_accept) {
        return false;
    }
    bool ok = false;
    if (xSemaphoreTakeRecursive(s_line_mux, pdMS_TO_TICKS(20)) == pdTRUE) {
        // All or nothing: a partial line would corrupt the CSV.
        if (s_accept && ring_buffer_free_space(&s_lines) >= len) {
            ring_buffer_write(&s_lines, (const uint8_t *)line, len);
            ok = true;
        }
        xSemaphoreGiveRecursive(s_line_mux);
    }
    if (!ok && s_accept) {
        s_log_drops++;
    }
    return ok;
}

// --- FORMAT.md -----------------------------------------------------------------

// Writes RECORDER_DIR/FORMAT.md if missing or different from the compiled-in
// text, so the card always describes its own files. Best-effort.
static void write_format_doc(void) {
    const char *path = RECORDER_DIR "/FORMAT.md";
    const char *doc = scan_log_format_doc();
    const size_t len = strlen(doc);
    bool same = false;
    FILE *f = fopen(path, "rb");
    if (f != NULL) {
        char *cur = malloc(len + 1);
        if (cur != NULL) {
            same = fread(cur, 1, len + 1, f) == len && memcmp(cur, doc, len) == 0;
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
    fwrite(doc, 1, len, f);
    fclose(f);
}

// --- SD card ---------------------------------------------------------------------

static bool mount_sd_card(void) {
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_1;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED; // 40 MHz; most cards support it

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.clk = PIN_SD_CLK;
    slot_config.cmd = PIN_SD_CMD;
    slot_config.d0 = PIN_SD_DAT0;
    slot_config.d1 = PIN_SD_DAT1;
    slot_config.d2 = PIN_SD_DAT2;
    slot_config.d3 = PIN_SD_DAT3;
    // Card-detect is polled ourselves (sd_cd_monitor_task): the socket's CD
    // polarity isn't known, and a wrong guess would block every mount.
    slot_config.cd = SDMMC_SLOT_NO_CD;
    slot_config.wp = SDMMC_SLOT_NO_WP;
    slot_config.width = 4;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 8, // session .csv/.log + marker save + downloads
        .allocation_unit_size = WRITE_CHUNK_SIZE,
    };

    esp_err_t err = esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &s_card);
    if (err != ESP_OK) {
        printf("recorder: SD mount failed: %s\n", esp_err_to_name(err));
        s_sd_mounted = false;
        return false;
    }
    s_sd_mounted = true;
    if (s_card != NULL) {
        printf("recorder: SD mounted -- negotiated bus parameters:\n");
        sdmmc_card_print_info(stdout, s_card);
    }
    s_last_error[0] = '\0'; // a stale "SD card not mounted" no longer applies

    if (mkdir(RECORDER_DIR, 0755) != 0 && errno != EEXIST) {
        printf("recorder: mkdir %s failed (errno=%d %s)\n", RECORDER_DIR, errno, strerror(errno));
    }
    write_format_doc();
    return true;
}

static void sd_cd_monitor_task(void *arg) {
    (void)arg;
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
    s_marker_mutex = xSemaphoreCreateMutex();
    s_line_mux = xSemaphoreCreateRecursiveMutex();
    s_lines_ok = ring_buffer_init(&s_lines, LINE_RING_BYTES, MALLOC_CAP_SPIRAM);
    if (!s_lines_ok) {
        printf("recorder: no memory for the line buffer -- logging unavailable\n");
    }
    mount_sd_card(); // failure is fine -- just means "no card yet"
    xTaskCreate(sd_cd_monitor_task, "sd_cd_monitor", 3072, NULL, 4, NULL);
    return s_sd_mounted;
}

bool recorder_is_sd_mounted(void) {
    return s_sd_mounted;
}

bool recorder_get_sd_space(uint64_t *total_bytes, uint64_t *free_bytes) {
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
    if (total_bytes) *total_bytes = cached_total;
    if (free_bytes) *free_bytes = cached_free;
    return true;
}

bool recorder_remount(void) {
    if (s_state != REC_IDLE) {
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

// --- Markers -----------------------------------------------------------------------

// Rewrites <base>.mark from the in-RAM list (caller holds s_marker_mutex).
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
    fprintf(mf, "id,time_s,uptime_ms,unix_time_s,note\n");
    for (int i = 0; i < s_marker_count; i++) {
        char unix_col[24] = "";
        if (s_markers[i].wall_unix_s > 0) {
            snprintf(unix_col, sizeof(unix_col), "%lld", (long long)s_markers[i].wall_unix_s);
        }
        fprintf(mf, "%u,%.3f,%lld,%s,\"%s\"\n", (unsigned)s_markers[i].id, s_markers[i].elapsed_s,
                (long long)(s_markers[i].at_us / 1000), unix_col, s_markers[i].note);
    }
    fflush(mf);
    fsync(fileno(mf));
    fclose(mf);
}

// Strips characters that would break a quoted CSV field and length-caps.
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
        double elapsed = s_session_start_us > 0 ? (double)(t - s_session_start_us) / 1e6 : 0.0;
        if (elapsed < 0) elapsed = 0;
        struct tm tm_info;
        int64_t wall = 0;
        if (wallclock_is_valid(&tm_info)) {
            wall = (int64_t)time(NULL) - (int64_t)((esp_timer_get_time() - t) / 1000000);
        }
        recorder_marker_t *m = &s_markers[s_marker_count++];
        m->id = s_next_marker_id++;
        m->elapsed_s = elapsed;
        m->at_us = t;
        m->wall_unix_s = wall;
        m->note[0] = '\0';
        write_marker_sidecar();
        scan_log_marker_event(m, "add");
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
            scan_log_marker_event(&s_markers[i], "note");
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
            recorder_marker_t gone = s_markers[i];
            for (int j = i; j < s_marker_count - 1; j++) {
                s_markers[j] = s_markers[j + 1];
            }
            s_marker_count--;
            write_marker_sidecar();
            scan_log_marker_event(&gone, "delete");
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
    return s_marker_count;
}

// --- Control / status ------------------------------------------------------------------

bool recorder_start_writer_task(void) {
    // Core 1, below the accelerometer drain (20), above the BLE processing task (8).
    return xTaskCreatePinnedToCore(sd_writer_task, "sd_writer", 8192, NULL, 12, NULL, 1) == pdPASS;
}

bool recorder_request_start(const char *name_tag) {
    if (s_state != REC_IDLE) {
        set_error("already logging");
        return false;
    }
    if (!s_sd_mounted) {
        set_error("SD card not mounted");
        return false;
    }
    if (!s_lines_ok) {
        set_error("no memory for the log buffer");
        return false;
    }
    s_last_error[0] = '\0';
    s_last_notice[0] = '\0';
    snprintf(s_pending_tag, sizeof(s_pending_tag), "%s", name_tag != NULL ? name_tag : "");
    s_start_requested = true;
    return true;
}

void recorder_request_stop(void) {
    if (s_state == REC_RECORDING) {
        // Drop a stale give from an earlier fire-and-forget stop, so the next
        // recorder_wait_for_stop() waits for *this* stop.
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

uint64_t recorder_get_rows_written(void) {
    return s_rows_written;
}

double recorder_get_elapsed_s(void) {
    if (s_session_start_us == 0) {
        return 0;
    }
    int64_t end = (s_state != REC_IDLE || s_session_end_us == 0) ? esp_timer_get_time() : s_session_end_us;
    return (double)(end - s_session_start_us) / 1e6;
}

float recorder_get_bytes_per_s(void) {
    if (s_state == REC_IDLE) {
        return 0;
    }
    if (s_rate_bps > 0) {
        return s_rate_bps;
    }
    double el = recorder_get_elapsed_s(); // first 10 s: average since start
    return el > 0.5 ? (float)(s_bytes_written / el) : 0;
}

uint32_t recorder_get_log_drops(void) {
    return s_log_drops;
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
