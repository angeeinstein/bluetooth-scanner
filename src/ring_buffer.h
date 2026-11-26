// Generic byte-oriented single-producer/single-consumer ring buffer.
//
// No locking: correct as long as exactly one task ever calls _write() and
// exactly one (possibly different) task ever calls _read(). Both the
// accelerometer drain task and the SD writer task run pinned to the same
// core (see accel_capture.c / recorder.c), so plain `volatile` indices are
// sufficient -- no cross-core memory barrier is needed here.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *buf;
    size_t capacity;
    volatile size_t head; // next byte the producer will write
    volatile size_t tail; // next byte the consumer will read
} ring_buffer_t;

// Allocates `capacity` bytes with the given heap_caps (e.g. MALLOC_CAP_SPIRAM)
// and initializes `rb`. Returns false on allocation failure.
bool ring_buffer_init(ring_buffer_t *rb, size_t capacity, uint32_t malloc_caps);

size_t ring_buffer_free_space(const ring_buffer_t *rb);
size_t ring_buffer_used_space(const ring_buffer_t *rb);

// Writes up to `len` bytes. Returns the number actually written (less than
// `len` if the buffer doesn't have enough free space) -- the caller decides
// whether a short write means "drop the rest and count it" or is an error.
size_t ring_buffer_write(ring_buffer_t *rb, const uint8_t *data, size_t len);

// Reads up to `len` bytes. Returns the number actually read.
size_t ring_buffer_read(ring_buffer_t *rb, uint8_t *data, size_t len);
