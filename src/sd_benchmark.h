#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    SD_BENCH_IDLE,
    SD_BENCH_WRITING,
    SD_BENCH_READING,
    SD_BENCH_DONE,
    SD_BENCH_FAILED,
} sd_bench_state_t;

typedef struct {
    sd_bench_state_t state;
    size_t total_bytes;
    size_t bytes_done; // progress within the current phase (write or read)
    float write_mb_s;
    float read_mb_s;
    uint32_t worst_chunk_ms; // slowest single 32KB write -- catches wear-leveling/GC stalls
    bool checksum_ok;
    char error[64];
} sd_bench_result_t;

// Starts the benchmark as a background task (never blocks the caller).
// Writes/reads/verifies ~64MB of synthetic data to a temp file, deleted
// afterwards. Returns false if one is already running or the SD isn't
// mounted.
bool sd_benchmark_start(void);

void sd_benchmark_get_result(sd_bench_result_t *out);
