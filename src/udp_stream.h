#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool enabled;
    char target_ip[16];
    uint16_t target_port;
    uint64_t packets_sent;
    uint64_t bytes_sent;
    uint32_t drops; // accel_capture's stream-ring drops, mirrored here for convenience
} udp_stream_status_t;

// Starts the always-running UDP send task (drains the stream ring
// continuously, discarding while disabled -- see accel_capture_get_stream_ring()).
// Call once at boot, after accel_capture_start().
bool udp_stream_init(void);

// Enables streaming to the given target, resetting the packet sequence
// number and sample index (so a receiver can tell a fresh session started).
// Returns false if `ip` doesn't parse as a dotted-decimal IPv4 address.
bool udp_stream_start(const char *ip, uint16_t port);

// Disables streaming (the send task keeps running and draining/discarding
// the ring so it never backs up -- this only stops it from calling sendto()).
void udp_stream_stop(void);

void udp_stream_get_status(udp_stream_status_t *out);
