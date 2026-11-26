#include "ring_buffer.h"

#include <string.h>

#include "esp_heap_caps.h"

bool ring_buffer_init(ring_buffer_t *rb, size_t capacity, uint32_t malloc_caps) {
    rb->buf = heap_caps_malloc(capacity, malloc_caps);
    if (rb->buf == NULL) {
        return false;
    }
    rb->capacity = capacity;
    rb->head = 0;
    rb->tail = 0;
    return true;
}

size_t ring_buffer_used_space(const ring_buffer_t *rb) {
    size_t head = rb->head;
    size_t tail = rb->tail;
    return (head >= tail) ? (head - tail) : (rb->capacity - tail + head);
}

size_t ring_buffer_free_space(const ring_buffer_t *rb) {
    // Reserve one byte to distinguish full from empty (head==tail is empty).
    return rb->capacity - 1 - ring_buffer_used_space(rb);
}

size_t ring_buffer_write(ring_buffer_t *rb, const uint8_t *data, size_t len) {
    // All-or-nothing: every caller of this ring pushes fixed-size records
    // (accel sample triplets, preview points) that consumers read back in
    // matching fixed-size chunks. A partial write (the old behavior here --
    // write whatever fits, even a few bytes of a record) silently shifts
    // every subsequent record's byte alignment by however much was short,
    // since the consumer has no framing to detect the skew. That produced
    // exactly the symptom seen on real hardware: once the (small) UDP
    // stream ring started running near-full, later samples decoded as
    // near-random values because their bytes were sourced from the wrong
    // positions -- visible as full-scale noise in the raw UDP feed despite
    // looking fine in the heavily-averaged decimated preview.
    size_t avail = ring_buffer_free_space(rb);
    if (len == 0 || len > avail) {
        return 0;
    }

    size_t head = rb->head;
    size_t first_chunk = rb->capacity - head;
    if (first_chunk > len) {
        first_chunk = len;
    }
    memcpy(rb->buf + head, data, first_chunk);
    size_t remaining = len - first_chunk;
    if (remaining > 0) {
        memcpy(rb->buf, data + first_chunk, remaining);
    }

    rb->head = (head + len) % rb->capacity;
    return len;
}

size_t ring_buffer_read(ring_buffer_t *rb, uint8_t *data, size_t len) {
    size_t avail = ring_buffer_used_space(rb);
    size_t to_read = (len < avail) ? len : avail;
    if (to_read == 0) {
        return 0;
    }

    size_t tail = rb->tail;
    size_t first_chunk = rb->capacity - tail;
    if (first_chunk > to_read) {
        first_chunk = to_read;
    }
    memcpy(data, rb->buf + tail, first_chunk);
    size_t remaining = to_read - first_chunk;
    if (remaining > 0) {
        // Wrapped: the rest of the data is at the start of the buffer.
        memcpy(data + first_chunk, rb->buf, remaining);
    }

    rb->tail = (tail + to_read) % rb->capacity;
    return to_read;
}
