#include "csv_export.h"

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h> // fsync

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h" // esp_vfs_fat_info (free-space check)
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "recorder.h"
#include "recording_format.h"

#define MOUNT_POINT "/sdcard"
#define REC_DIR MOUNT_POINT "/rec"

// How many samples to convert per batch: read raw_buf, format into
// text_buf, one fwrite() per batch (not one per line -- far fewer FATFS
// calls for the same amount of data).
#define BATCH_SAMPLES 2048
#define BYTES_PER_TEXT_LINE 96 // generous upper bound, see append() usage below

#define CSV_HEADER "sample_index,time_s,unix_time_s,x_g,y_g,z_g\n"

// Periodic durability sync (mirrors recorder.c's FSYNC_INTERVAL_MS): commit
// data + FATFS metadata every few seconds so a power loss mid-conversion
// doesn't orphan everything written so far. The resume checkpoint (below) is
// rewritten at the same cadence.
#define CSV_FSYNC_INTERVAL_MS 5000

// Conservative per-sample output-size estimates for the pre-flight free-space
// check, from the widest realistic line the loop below emits (see the append()
// format strings): ~67 bytes with the unix_time_s column populated, ~50
// without. Rounded up for margin; the in-loop fwrite() check is the backstop
// if the estimate turns out optimistic and the card fills anyway.
#define EST_CSV_BYTES_PER_SAMPLE_WALL 72
#define EST_CSV_BYTES_PER_SAMPLE_NOWALL 52

// Resuming an interrupted conversion has to fseek() back into the partial
// output, and this build's off_t is 32-bit, so the resume offset is capped
// below the 2GB signed limit. A conversion whose *finished* CSV would exceed
// this is treated as a big, deliberate job: an interrupted one is deleted at
// boot rather than auto-resumed or auto-restarted, so re-running it is always
// an explicit choice. (2000 MiB, comfortably under LONG_MAX.)
#define CSV_RESUME_MAX_BYTES (2000ULL * 1024 * 1024)

// Checkpoint sidecar written next to the .csv.tmp. Lets an interrupted
// conversion resume in O(1) (no need to re-scan a multi-GB partial file to
// find where it stopped). Rewritten + fsync'd after each data fsync, so it
// never points past durable data; if it lags the crash slightly, resume just
// re-converts a few seconds of samples (idempotent -- byte-identical output).
#define CSV_PROG_MAGIC "CVPG"
#define CSV_PROG_VERSION 1u
typedef struct {
    char magic[4];
    uint32_t version;
    uint64_t bin_size;      // source .bin size when this run began (validates the pair)
    uint64_t samples_done;  // resume from this input sample
    uint64_t bytes_written; // resume offset into the .csv.tmp
} csv_progress_t;

static volatile csv_export_state_t s_state = CSV_EXPORT_IDLE;
static csv_export_result_t s_result;
static char s_pending_filename[64];
static volatile int64_t s_start_us = 0;         // when the current run's write loop began (for ETA)
static volatile uint64_t s_run_start_sample = 0; // samples already done before this run (resume)
static volatile bool s_resume_mode = false;      // next run continues an existing .csv.tmp
static volatile bool s_cancel_requested = false; // stop the running conversion and delete its temp files

// An interrupted conversion found at boot but left un-resumed (battery was
// low). Surfaced to the dashboard for a manual Resume / Discard. Written once
// from the boot scan and cleared when acted on -- effectively single-threaded.
static bool s_pending = false;
static char s_pending_name[64]; // source .bin filename

// Same bounded-accumulation pattern used in the dashboard's JSON builder:
// vsnprintf's return value can exceed remaining space on truncation, so we
// clamp `off` against `buf_len` on every call rather than trusting it blindly.
static size_t append(char *buf, size_t buf_len, size_t off, const char *fmt, ...) {
    if (off >= buf_len) {
        return off;
    }
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf + off, buf_len - off, fmt, args);
    va_end(args);
    if (n < 0) {
        return off;
    }
    return off + (size_t)n;
}

// Builds the sibling .csv / .csv.tmp / .csv.prog paths for a recording's .bin
// filename (e.g. "rec_00001.bin"). Any NULL out-pointer is skipped.
static void derive_paths(const char *bin_name, char *csv, size_t cs, char *tmp, size_t ts,
                         char *prog, size_t ps) {
    char base[64];
    snprintf(base, sizeof(base), "%s", bin_name);
    size_t bl = strlen(base);
    if (bl > 4 && strcmp(base + bl - 4, ".bin") == 0) {
        base[bl - 4] = '\0';
    }
    if (csv) {
        snprintf(csv, cs, "%s/%s.csv", REC_DIR, base);
    }
    if (tmp) {
        snprintf(tmp, ts, "%s/%s.csv.tmp", REC_DIR, base);
    }
    if (prog) {
        snprintf(prog, ps, "%s/%s.csv.prog", REC_DIR, base);
    }
}

// Best-effort: a missing/failed checkpoint just means the next boot can't
// resume this conversion (it'll be discarded), never a corrupt output.
static void write_progress(const char *prog_path, uint64_t bin_size, uint64_t samples,
                           uint64_t bytes) {
    FILE *pf = fopen(prog_path, "wb");
    if (pf == NULL) {
        return;
    }
    csv_progress_t p;
    memcpy(p.magic, CSV_PROG_MAGIC, 4);
    p.version = CSV_PROG_VERSION;
    p.bin_size = bin_size;
    p.samples_done = samples;
    p.bytes_written = bytes;
    fwrite(&p, sizeof(p), 1, pf);
    fflush(pf);
    fsync(fileno(pf));
    fclose(pf);
}

static void run_export(void) {
    bool resume = s_resume_mode;
    s_resume_mode = false;

    char bin_path[160];
    snprintf(bin_path, sizeof(bin_path), "%s/%s", REC_DIR, s_pending_filename);

    FILE *in = fopen(bin_path, "rb");
    if (in == NULL) {
        snprintf(s_result.error, sizeof(s_result.error), "open failed (errno=%d %s)", errno,
                 strerror(errno));
        s_state = CSV_EXPORT_FAILED;
        return;
    }

    recording_header_t hdr;
    if (fread(&hdr, sizeof(hdr), 1, in) != 1 || memcmp(hdr.magic, RECORDING_MAGIC, 4) != 0) {
        snprintf(s_result.error, sizeof(s_result.error), "bad or missing header");
        fclose(in);
        s_state = CSV_EXPORT_FAILED;
        return;
    }

    // Sample data starts after the header + the (v3) sync-checkpoint table; a v2
    // file has sync_table_bytes == 0, so this stays correct for both.
    long data_off = (long)sizeof(hdr) + (long)hdr.sync_table_bytes;

    fseek(in, 0, SEEK_END);
    long file_size = ftell(in);
    if (file_size < data_off) {
        snprintf(s_result.error, sizeof(s_result.error), "truncated file");
        fclose(in);
        s_state = CSV_EXPORT_FAILED;
        return;
    }
    s_result.total_samples = (uint64_t)(file_size - data_off) / 6;

    bool wall_valid = hdr.wall_time_valid != 0;
    uint32_t bytes_per_sample =
        wall_valid ? EST_CSV_BYTES_PER_SAMPLE_WALL : EST_CSV_BYTES_PER_SAMPLE_NOWALL;

    char csv_path[192], tmp_path[192], prog_path[192];
    derive_paths(s_pending_filename, csv_path, sizeof(csv_path), tmp_path, sizeof(tmp_path),
                 prog_path, sizeof(prog_path));

    // Figure out where to start (0 for a fresh run, or the checkpoint for a
    // resume) before touching the output, so the free-space check and the
    // open mode are both driven off it.
    uint64_t start_sample = 0;
    uint64_t bytes_written = strlen(CSV_HEADER);

    if (resume) {
        csv_progress_t p;
        FILE *pf = fopen(prog_path, "rb");
        bool valid = pf != NULL && fread(&p, sizeof(p), 1, pf) == 1 &&
                     memcmp(p.magic, CSV_PROG_MAGIC, 4) == 0 && p.version == CSV_PROG_VERSION &&
                     p.bin_size == (uint64_t)file_size && p.samples_done <= s_result.total_samples &&
                     p.bytes_written <= CSV_RESUME_MAX_BYTES;
        if (pf != NULL) {
            fclose(pf);
        }
        struct stat tst;
        if (valid && (stat(tmp_path, &tst) != 0 || (uint64_t)tst.st_size < p.bytes_written)) {
            valid = false; // the partial output is gone or shorter than the checkpoint
        }
        if (!valid) {
            snprintf(s_result.error, sizeof(s_result.error), "resume checkpoint invalid");
            fclose(in);
            s_state = CSV_EXPORT_FAILED;
            return;
        }
        start_sample = p.samples_done;
        bytes_written = p.bytes_written;
    }

    // Pre-flight free-space check on what's still to write. Best-effort -- if
    // the info call fails we proceed and rely on the in-loop fwrite() check.
    uint64_t total_bytes = 0, free_bytes = 0;
    uint64_t est_bytes = (s_result.total_samples - start_sample) * bytes_per_sample + 64;
    if (esp_vfs_fat_info(MOUNT_POINT, &total_bytes, &free_bytes) == ESP_OK &&
        free_bytes < est_bytes) {
        snprintf(s_result.error, sizeof(s_result.error), "need ~%lluMB, %lluMB free",
                 (unsigned long long)(est_bytes >> 20), (unsigned long long)(free_bytes >> 20));
        fclose(in);
        s_state = CSV_EXPORT_FAILED;
        return;
    }

    int16_t *raw_buf = heap_caps_malloc(BATCH_SAMPLES * 3 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    char *text_buf = heap_caps_malloc(BATCH_SAMPLES * BYTES_PER_TEXT_LINE, MALLOC_CAP_SPIRAM);
    if (raw_buf == NULL || text_buf == NULL) {
        snprintf(s_result.error, sizeof(s_result.error), "out of memory");
        heap_caps_free(raw_buf);
        heap_caps_free(text_buf);
        fclose(in);
        s_state = CSV_EXPORT_FAILED;
        return; // leaves any .tmp/.prog in place -- safe to retry/resume later
    }

    // Open the staging file. Fresh: create + header + initial checkpoint.
    // Resume: reopen and seek back to the checkpoint offset; writing from there
    // reproduces byte-identical output over any post-checkpoint remnant and
    // continues past it, so the final file is always complete and correct.
    FILE *out = NULL;
    if (resume) {
        out = fopen(tmp_path, "r+b");
        if (out != NULL && fseek(out, (long)bytes_written, SEEK_SET) != 0) {
            fclose(out);
            out = NULL;
        }
    } else {
        out = fopen(tmp_path, "w");
    }
    if (out == NULL) {
        snprintf(s_result.error, sizeof(s_result.error), "csv open failed (errno=%d %s)", errno,
                 strerror(errno));
        heap_caps_free(raw_buf);
        heap_caps_free(text_buf);
        fclose(in);
        s_state = CSV_EXPORT_FAILED;
        return;
    }
    int out_fd = fileno(out);
    fseek(in, data_off + (long)(start_sample * 6), SEEK_SET);
    if (!resume) {
        fputs(CSV_HEADER, out);
        write_progress(prog_path, (uint64_t)file_size, 0, bytes_written);
    }

    float sens_g = hdr.sensitivity_mg_per_lsb / 1000.0f;
    double sample_period_s = hdr.sample_rate_hz > 0 ? 1.0 / hdr.sample_rate_hz : 0.0;
    double wall_start_s = (double)hdr.wall_time_unix_s;
    uint64_t sample_index = start_sample;
    size_t text_cap = (size_t)BATCH_SAMPLES * BYTES_PER_TEXT_LINE;

    s_run_start_sample = start_sample;
    s_result.samples_done = start_sample;
    s_start_us = esp_timer_get_time();
    int64_t last_fsync_us = s_start_us;
    bool write_error = false;
    bool cancelled = false;

    for (;;) {
        if (s_cancel_requested) {
            cancelled = true;
            break;
        }
        size_t got = fread(raw_buf, sizeof(int16_t) * 3, BATCH_SAMPLES, in);
        if (got == 0) {
            break;
        }

        size_t off = 0;
        for (size_t i = 0; i < got; i++) {
            float x = raw_buf[i * 3 + 0] * sens_g;
            float y = raw_buf[i * 3 + 1] * sens_g;
            float z = raw_buf[i * 3 + 2] * sens_g;
            double time_s = sample_index * sample_period_s;
            // unix_time_s left blank (not a fabricated 0/NaN) when the
            // recording started before any wall clock -- NTP or browser --
            // had synced; most tools read a blank numeric CSV field as NaN.
            if (wall_valid) {
                off = append(text_buf, text_cap, off, "%llu,%.6f,%.6f,%.4f,%.4f,%.4f\n",
                             (unsigned long long)sample_index, time_s, wall_start_s + time_s,
                             (double)x, (double)y, (double)z);
            } else {
                off = append(text_buf, text_cap, off, "%llu,%.6f,,%.4f,%.4f,%.4f\n",
                             (unsigned long long)sample_index, time_s, (double)x, (double)y,
                             (double)z);
            }
            sample_index++;
        }
        if (fwrite(text_buf, 1, off, out) != off) {
            // Short write -- almost always a full card. Stop cleanly with a
            // clear error instead of silently producing a truncated file.
            snprintf(s_result.error, sizeof(s_result.error), "write failed (card full?)");
            write_error = true;
            break;
        }
        bytes_written += off;
        s_result.samples_done = sample_index;

        int64_t now_us = esp_timer_get_time();
        if (now_us - last_fsync_us >= (int64_t)CSV_FSYNC_INTERVAL_MS * 1000) {
            fflush(out);
            fsync(out_fd);
            write_progress(prog_path, (uint64_t)file_size, sample_index, bytes_written);
            last_fsync_us = now_us;
        }

        // Low-priority background job -- give other tasks (including the SD
        // writer, if a new recording starts) a chance every batch. Raw tick
        // count, not pdMS_TO_TICKS(1): that macro truncates rather than
        // rounds, and at this build's CONFIG_FREERTOS_HZ=100 (10ms/tick),
        // pdMS_TO_TICKS(1) evaluates to 0 ticks -- vTaskDelay(0) doesn't
        // reliably yield to a lower-priority task (see udp_stream.c for the
        // watchdog-starvation bug this exact pattern caused there).
        vTaskDelay(1);
    }

    heap_caps_free(raw_buf);
    heap_caps_free(text_buf);
    fclose(in);
    fflush(out);
    fsync(out_fd);
    fclose(out);

    if (cancelled) {
        // User asked to stop -- throw the partial away (that's the point).
        remove(tmp_path);
        remove(prog_path);
        s_state = CSV_EXPORT_CANCELLED;
        return;
    }

    if (write_error) {
        // Keep .tmp + .prog: a later boot (or a manual resume once space is
        // freed) can pick up from the last checkpoint. The user sees the error
        // and can Discard from the dashboard if they'd rather start over.
        s_state = CSV_EXPORT_FAILED;
        return;
    }

    // Publish. rename first (FATFS won't overwrite, so clear any stale target),
    // then drop the checkpoint -- if we crash between the two, a lone .prog with
    // no .tmp is harmless litter the boot scan sweeps up.
    remove(csv_path);
    if (rename(tmp_path, csv_path) != 0) {
        snprintf(s_result.error, sizeof(s_result.error), "rename failed (errno=%d)", errno);
        s_state = CSV_EXPORT_FAILED;
        return;
    }
    remove(prog_path);
    s_state = CSV_EXPORT_DONE;
}

static void export_task(void *arg) {
    run_export();
    vTaskDelete(NULL);
}

static bool start_conversion(const char *filename, bool resume) {
    if (s_state == CSV_EXPORT_RUNNING) {
        return false;
    }
    if (recorder_is_recording()) {
        return false;
    }
    if (filename == NULL || filename[0] == '\0') {
        return false;
    }

    memset(&s_result, 0, sizeof(s_result));
    s_result.eta_s = -1;
    s_start_us = 0;
    s_run_start_sample = 0;
    s_resume_mode = resume;
    s_cancel_requested = false;
    strncpy(s_pending_filename, filename, sizeof(s_pending_filename) - 1);
    s_pending_filename[sizeof(s_pending_filename) - 1] = '\0';
    strncpy(s_result.filename, filename, sizeof(s_result.filename) - 1);
    s_result.filename[sizeof(s_result.filename) - 1] = '\0';
    if (s_pending && strcmp(s_pending_name, filename) == 0) {
        s_pending = false; // it's being acted on now
    }
    s_state = CSV_EXPORT_RUNNING;

    BaseType_t created = xTaskCreate(export_task, "csv_export", 8192, NULL, 3, NULL);
    if (created != pdPASS) {
        s_state = CSV_EXPORT_FAILED;
        return false;
    }
    return true;
}

bool csv_export_start(const char *filename) {
    return start_conversion(filename, false);
}

bool csv_export_resume(const char *filename) {
    if (!s_pending || filename == NULL || strcmp(s_pending_name, filename) != 0) {
        return false;
    }
    return start_conversion(filename, true);
}

bool csv_export_discard(const char *filename) {
    if (!s_pending || filename == NULL || strcmp(s_pending_name, filename) != 0) {
        return false;
    }
    char tmp_path[192], prog_path[192];
    derive_paths(filename, NULL, 0, tmp_path, sizeof(tmp_path), prog_path, sizeof(prog_path));
    remove(tmp_path);
    remove(prog_path);
    s_pending = false;
    return true;
}

bool csv_export_cancel(void) {
    if (s_state != CSV_EXPORT_RUNNING) {
        return false;
    }
    s_cancel_requested = true; // the worker sees this at its next batch, cleans up, and exits
    return true;
}

bool csv_export_get_pending(char *out_name, size_t len) {
    if (!s_pending || out_name == NULL || len == 0) {
        return false;
    }
    strncpy(out_name, s_pending_name, len - 1);
    out_name[len - 1] = '\0';
    return true;
}

void csv_export_resume_on_boot(bool battery_low) {
    if (s_state == CSV_EXPORT_RUNNING) {
        return;
    }

    // At most one conversion runs at a time, so at most one .csv.tmp can exist.
    // Capture its name; act after closing the dir (mutating it mid-iteration is
    // unsafe on FATFS).
    char tmp_name[64] = "";
    DIR *d = opendir(REC_DIR);
    if (d == NULL) {
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t nlen = strlen(e->d_name);
        if (nlen > 8 && nlen < sizeof(tmp_name) &&
            strcmp(e->d_name + nlen - 8, ".csv.tmp") == 0) {
            strcpy(tmp_name, e->d_name);
            break;
        }
    }
    closedir(d);
    if (tmp_name[0] == '\0') {
        return; // no interrupted conversion
    }

    // "rec_00001.csv.tmp" -> "rec_00001.bin"
    char bin_name[64];
    size_t base_len = strlen(tmp_name) - 8; // strip ".csv.tmp"
    snprintf(bin_name, sizeof(bin_name), "%.*s.bin", (int)base_len, tmp_name);

    char bin_path[160], tmp_path[192], prog_path[192];
    snprintf(bin_path, sizeof(bin_path), "%s/%s", REC_DIR, bin_name);
    derive_paths(bin_name, NULL, 0, tmp_path, sizeof(tmp_path), prog_path, sizeof(prog_path));

    struct stat st;
    if (stat(bin_path, &st) != 0) {
        remove(tmp_path); // source recording is gone -- nothing to resume
        remove(prog_path);
        return;
    }
    uint64_t bin_size = (uint64_t)st.st_size;
    uint64_t total_samples =
        bin_size > sizeof(recording_header_t) ? (bin_size - sizeof(recording_header_t)) / 6 : 0;

    // A >2GB finished CSV can't be byte-resumed here (32-bit off_t) and is a
    // deliberate, long job -- drop it and let the user re-trigger on purpose.
    if (total_samples * (uint64_t)EST_CSV_BYTES_PER_SAMPLE_WALL > CSV_RESUME_MAX_BYTES) {
        remove(tmp_path);
        remove(prog_path);
        printf("csv_export: dropped interrupted >2GB conversion of %s (re-trigger manually)\n",
               bin_name);
        return;
    }

    // No checkpoint -> can't resume reliably; discard the partial.
    struct stat pst;
    if (stat(prog_path, &pst) != 0) {
        remove(tmp_path);
        return;
    }

    if (battery_low) {
        // Leave everything in place; offer a manual Resume / Discard rather than
        // spending the little remaining charge auto-converting on a low cell.
        s_pending = true;
        strncpy(s_pending_name, bin_name, sizeof(s_pending_name) - 1);
        s_pending_name[sizeof(s_pending_name) - 1] = '\0';
        printf("csv_export: interrupted conversion of %s pending (battery low)\n", bin_name);
        return;
    }

    printf("csv_export: auto-resuming interrupted conversion of %s\n", bin_name);
    start_conversion(bin_name, true);
}

void csv_export_get_result(csv_export_result_t *out) {
    *out = s_result;
    out->state = s_state;

    // ETA from the average rate over *this run* (samples converted since it
    // started / elapsed). Using only this run's progress keeps a resumed
    // conversion's ETA honest, and extrapolating the average is self-correcting:
    // if the conversion stalls, elapsed grows while samples don't, so the ETA
    // climbs rather than claiming it's nearly done.
    out->eta_s = -1;
    if (out->state == CSV_EXPORT_RUNNING && s_start_us > 0 &&
        out->samples_done > s_run_start_sample && out->total_samples >= out->samples_done) {
        int64_t elapsed_us = esp_timer_get_time() - s_start_us;
        if (elapsed_us > 0) {
            uint64_t done_this_run = out->samples_done - s_run_start_sample;
            double per_sample_us = (double)elapsed_us / (double)done_this_run;
            uint64_t remaining = out->total_samples - out->samples_done;
            out->eta_s = (int32_t)((double)remaining * per_sample_us / 1e6);
        }
    }
}
