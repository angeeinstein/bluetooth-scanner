#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_http_server.h"

typedef struct {
    bool enabled;
    uint64_t frames_sent;
    uint64_t bytes_sent;
    uint32_t drops; // accel_capture's WS-stream-ring drops, mirrored here for convenience
} ws_stream_status_t;

// Starts the always-running send task (drains its own dedicated ring --
// see accel_capture_get_ws_stream_ring() -- continuously, discarding while
// disabled, same idiom as udp_stream.c). Call once at boot, after
// accel_capture_start().
bool ws_stream_init(void);

// Associates this module with the running httpd server so it can push
// frames to connected clients. Call once, right after httpd_start()
// succeeds in dashboard.c.
void ws_stream_set_server(httpd_handle_t server);

// Registers a newly-connected /wsstream client (call from the GET
// /wsstream websocket handshake handler, passing httpd_req_to_sockfd(req)).
// Broadcasts go to every currently-registered, still-live client -- no
// single-client limit, bounded only by a small fixed-size registry.
void ws_stream_register_client(int fd);

// Enables streaming, resetting the frame sequence number and sample index
// (so a client can tell a fresh session started).
bool ws_stream_start(void);

// Disables streaming (the send task keeps running and draining/discarding
// its ring so it never backs up -- this only stops it from pushing frames).
void ws_stream_stop(void);

void ws_stream_get_status(ws_stream_status_t *out);
