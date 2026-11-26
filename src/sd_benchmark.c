#include "sd_benchmark.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "accel_capture.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "recorder.h"

#define BENCH_CHUNK_SIZE (32 * 1024)
#define BENCH_TOTAL_BYTES (64ULL * 1024 * 1024)
#define BENCH_FILE "/sdcard/benchmark.tmp"

static volatile sd_bench_state_t s_state = SD_BENCH_IDLE;
static sd_bench_result_t s_result;

// Simple, fast, deterministic pattern -- not cryptographic, just needs to
// be cheap to generate and to catch bit-level corruption on read-back.
static uint32_t pattern_word(uint32_t index) {
    return index * 2654435761u + 0x9E3779B9u;
}

static void fill_chunk(uint8_t *chunk, size_t len, uint32_t start_index) {
    uint32_t *words = (uint32_t *)chunk;
    size_t num_words = len / sizeof(uint32_t);
    for (size_t i = 0; i < num_words; i++) {
        words[i] = pattern_word(start_index + (uint32_t)i);
    }
}

// Does the actual write/read/verify work. Every path is a plain `return` --
// the caller (benchmark_task) always resumes accelerometer capture and
// deletes the task afterwards, regardless of how this exits.
static void run_benchmark(uint8_t *chunk, size_t chunk_size) {
    // Raw POSIX open/write/read (not stdio fopen/fwrite): stdio would copy through
    // its own small buffer and flush the card in tiny pieces, so we'd never hand
    // the SDMMC controller a large, aligned, DMA-able block -- which is most of why
    // an earlier stdio version measured ~1 MB/s on a bus that negotiates 40 MHz /
    // 4-bit. write()/read() pass the whole block straight to FATFS, and with `chunk`
    // in DMA-capable internal RAM (see benchmark_task) it goes to the card by DMA.

    // --- Write phase ---
    s_state = SD_BENCH_WRITING;
    int fd = open(BENCH_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        if (errno == ENOSPC) {
            snprintf(s_result.error, sizeof(s_result.error),
                     "SD card full - free up space to run the benchmark");
        } else {
            snprintf(s_result.error, sizeof(s_result.error),
                     "failed to open temp file (errno=%d %s)", errno, strerror(errno));
        }
        printf("sd_benchmark: %s\n", s_result.error);
        s_state = SD_BENCH_FAILED;
        return;
    }

    uint32_t word_index = 0;
    uint32_t worst_chunk_us = 0;
    int64_t write_start_us = esp_timer_get_time();
    size_t written_total = 0;
    while (written_total < BENCH_TOTAL_BYTES) {
        fill_chunk(chunk, chunk_size, word_index);
        word_index += chunk_size / sizeof(uint32_t);

        int64_t chunk_start_us = esp_timer_get_time();
        ssize_t written = write(fd, chunk, chunk_size);
        int write_errno = errno; // capture before other calls perturb it
        int64_t chunk_us = esp_timer_get_time() - chunk_start_us;
        if ((uint32_t)chunk_us > worst_chunk_us) {
            worst_chunk_us = (uint32_t)chunk_us;
        }
        if (written != (ssize_t)chunk_size) {
            // A full card is the common cause here (the recorder fills the card on
            // purpose) -- report it plainly rather than as a cryptic short write.
            if (write_errno == ENOSPC) {
                snprintf(s_result.error, sizeof(s_result.error),
                         "SD card full - free up space to run the benchmark");
            } else {
                snprintf(s_result.error, sizeof(s_result.error),
                         "short write at %u bytes (errno=%d %s)", (unsigned)written_total,
                         write_errno, strerror(write_errno));
            }
            close(fd);
            unlink(BENCH_FILE); // don't leave the partial temp file eating space
            s_state = SD_BENCH_FAILED;
            return;
        }
        written_total += (size_t)written;
        s_result.bytes_done = written_total;
    }
    fsync(fd); // force the card to commit before we stop the clock
    close(fd);
    int64_t write_elapsed_us = esp_timer_get_time() - write_start_us;
    s_result.write_mb_s = (float)written_total / (1024.0f * 1024.0f) /
                           ((float)write_elapsed_us / 1000000.0f);
    s_result.worst_chunk_ms = worst_chunk_us / 1000;

    // --- Read + verify phase ---
    s_state = SD_BENCH_READING;
    fd = open(BENCH_FILE, O_RDONLY);
    if (fd < 0) {
        snprintf(s_result.error, sizeof(s_result.error),
                 "failed to reopen temp file (errno=%d %s)", errno, strerror(errno));
        printf("sd_benchmark: %s\n", s_result.error);
        s_state = SD_BENCH_FAILED;
        return;
    }

    word_index = 0;
    size_t read_total = 0;
    int64_t read_start_us = esp_timer_get_time();
    while (read_total < BENCH_TOTAL_BYTES) {
        ssize_t got = read(fd, chunk, chunk_size);
        if (got != (ssize_t)chunk_size) {
            snprintf(s_result.error, sizeof(s_result.error), "short read at %u bytes",
                     (unsigned)read_total);
            s_result.checksum_ok = false;
            break;
        }
        uint32_t *words = (uint32_t *)chunk;
        size_t num_words = chunk_size / sizeof(uint32_t);
        for (size_t i = 0; i < num_words; i++) {
            if (words[i] != pattern_word(word_index + (uint32_t)i)) {
                s_result.checksum_ok = false;
                break;
            }
        }
        word_index += (uint32_t)num_words;
        read_total += (size_t)got;
        s_result.bytes_done = read_total;
        if (!s_result.checksum_ok) {
            break;
        }
    }
    close(fd);
    int64_t read_elapsed_us = esp_timer_get_time() - read_start_us;
    s_result.read_mb_s =
        (float)read_total / (1024.0f * 1024.0f) / ((float)read_elapsed_us / 1000000.0f);

    unlink(BENCH_FILE);
    s_state = s_result.checksum_ok ? SD_BENCH_DONE : SD_BENCH_FAILED;
}

static void benchmark_task(void *arg) {
    memset(&s_result, 0, sizeof(s_result));
    s_result.total_bytes = BENCH_TOTAL_BYTES;
    s_result.checksum_ok = true;

    // MALLOC_CAP_DMA forces DMA-capable *internal* RAM. This matters: with
    // CONFIG_SPIRAM_USE_MALLOC and a 16KB always-internal threshold, a plain
    // MALLOC_CAP_DEFAULT buffer lands in PSRAM, which the SDMMC DMA can't reach --
    // the driver then bounces every transfer through a small internal buffer,
    // collapsing throughput. An internal buffer lets it DMA directly.
    //
    // Internal DMA RAM is scarce at runtime (Wi-Fi + HTTP server are up), so a full
    // 32KB block may not be free/contiguous. Take the largest we can get down to a
    // 4KB floor -- even 8KB is a 16-sector transfer, still far better than a bounce.
    size_t chunk_size = BENCH_CHUNK_SIZE;
    uint8_t *chunk = NULL;
    while (chunk_size >= 4096) {
        chunk = heap_caps_malloc(chunk_size, MALLOC_CAP_DMA);
        if (chunk != NULL) {
            break;
        }
        chunk_size /= 2;
    }
    if (chunk == NULL) {
        snprintf(s_result.error, sizeof(s_result.error), "out of memory");
        s_state = SD_BENCH_FAILED;
        vTaskDelete(NULL);
        return;
    }

    // Measure the card's own ceiling, not contention with the always-on
    // accelerometer capture -- that's what "just to see what SD cards can
    // do" means (a real recording session will have some contention, but
    // the 4MB ring buffer is what absorbs that, not raw SD speed).
    accel_capture_pause();
    run_benchmark(chunk, chunk_size);
    accel_capture_resume();

    heap_caps_free(chunk);
    vTaskDelete(NULL);
}

bool sd_benchmark_start(void) {
    if (s_state == SD_BENCH_WRITING || s_state == SD_BENCH_READING) {
        return false;
    }
    if (!recorder_is_sd_mounted()) {
        return false;
    }
    BaseType_t created =
        xTaskCreatePinnedToCore(benchmark_task, "sd_bench", 4096, NULL, 4, NULL, 1);
    return created == pdPASS;
}

void sd_benchmark_get_result(sd_bench_result_t *out) {
    *out = s_result;
    out->state = s_state;
}
