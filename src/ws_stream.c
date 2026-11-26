#include "ws_stream.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "accel_capture.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "ring_buffer.h"

// 512 samples/frame: WebSocket runs over TCP, so unlike udp_stream.c's
// UDP-MTU-sized packets there's no fragmentation concern sizing this --
// chosen per the ChatGPT-researched design the user brought in, matching
// ~52 frames/sec at the fixed 26.7kHz sample rate.
#define WS_FRAME_SAMPLES 512
#define WS_HEADER_SIZE 16
#define WS_PAYLOAD_SIZE (WS_FRAME_SAMPLES * 6)
#define WS_FRAME_SIZE (WS_HEADER_SIZE + WS_PAYLOAD_SIZE)
#define WS_STREAM_MAGIC 0xAC

// Small, fixed registry of /wsstream client fds -- well under the server's
// overall 7-socket ceiling (shared with /ws and plain HTTP requests), so
// this never needs to be dynamic.
#define WS_STREAM_MAX_CLIENTS 4

// Backpressure cap on outstanding async sends. Each queued send holds a
// malloc'd copy of the frame (~3 KB) until the httpd task actually delivers it.
// A browser on Wi-Fi can't keep up with the full 26.7 kHz stream, so
// httpd_ws_send_frame_async drains slower than send_task produces -- without a
// cap the queued copies pile up (observed: heap fell from ~2.4 MB to ~220 KB
// over ~an hour, then the stream stalled). We simply drop frames while too many
// are already in flight; the live view was already dropping most of them anyway.
#define WS_MAX_INFLIGHT 8
static atomic_int s_inflight; // zero-initialised (static storage)

typedef struct {
    httpd_handle_t hd;
    int fd;
    uint8_t *payload;
    size_t len;
} ws_send_arg_t;

static ring_buffer_t *s_ring = NULL;
static SemaphoreHandle_t s_mutex;
static uint8_t s_frame[WS_FRAME_SIZE];

static volatile bool s_enabled = false; // fast-path flag, read every frame by send_task
static httpd_handle_t s_server = NULL;

// Everything below is protected by s_mutex.
static int s_client_fds[WS_STREAM_MAX_CLIENTS];
static int s_client_count = 0;
static uint32_t s_seq = 0;
static uint64_t s_sample_index = 0;
static ws_stream_status_t s_status; // .enabled/.drops filled fresh in ws_stream_get_status()

// Caller must hold s_mutex -- reads s_seq/s_sample_index/accel config directly.
static void build_header(uint8_t *buf) {
    uint8_t fs_code = (uint8_t)accel_capture_get_fs() & 0x03;
    uint8_t flags = fs_code | (accel_capture_get_lpf2() ? 0x04 : 0x00);
    uint16_t sample_count = WS_FRAME_SAMPLES;

    buf[0] = WS_STREAM_MAGIC;
    buf[1] = flags;
    // Same little-endian convention as udp_stream.c -- ESP32 is
    // little-endian and so is essentially every realistic browser/host.
    memcpy(buf + 2, &sample_count, sizeof(sample_count));
    memcpy(buf + 4, &s_seq, sizeof(s_seq));
    memcpy(buf + 8, &s_sample_index, sizeof(s_sample_index));
}

static void ws_stream_async_send(void *arg) {
    ws_send_arg_t *a = (ws_send_arg_t *)arg;
    // Re-check the socket is still a live websocket *at send time*. This runs on
    // the httpd task -- the same task that processes the client's close frame --
    // so by the time this fires, a tab that has just been closed is already gone.
    // Frames are queued up to WS_MAX_INFLIGHT deep in broadcast_locked() (checked
    // only at queue time); without this re-check one of those in-flight frames
    // gets delivered after the browser's close handshake, which the browser
    // rejects with "Data frame received after close" (harmless, but noisy).
    if (httpd_ws_get_fd_info(a->hd, a->fd) == HTTPD_WS_CLIENT_WEBSOCKET) {
        httpd_ws_frame_t frame = {
            .final = true,
            .fragmented = false,
            .type = HTTPD_WS_TYPE_BINARY,
            .payload = a->payload,
            .len = a->len,
        };
        // A dropped Wi-Fi client's TCP socket isn't torn down instantly -- the
        // peer just stops ACKing, so the send buffer fills and this starts
        // returning EAGAIN every attempt. httpd_ws_get_fd_info() won't see that
        // on its own (the session looks "live" until the socket is actually
        // closed), so a send failure here has to trigger that close itself,
        // otherwise this spins forever.
        if (httpd_ws_send_frame_async(a->hd, a->fd, &frame) != ESP_OK) {
            httpd_sess_trigger_close(a->hd, a->fd);
        }
    }
    free(a->payload);
    free(a);
    atomic_fetch_sub(&s_inflight, 1);
}

// Broadcasts to every registered client that's still a live websocket,
// pruning any that aren't (disconnected since they last registered).
// Caller must hold s_mutex.
static void broadcast_locked(const uint8_t *data, size_t len) {
    if (s_server == NULL) {
        return;
    }
    // Backpressure: if the httpd task hasn't drained the previous sends yet,
    // drop this frame rather than queue another malloc'd copy behind them.
    // Bounds the streaming path's heap use to ~WS_MAX_INFLIGHT frames.
    bool over_limit = atomic_load(&s_inflight) >= WS_MAX_INFLIGHT;
    int kept = 0;
    for (int i = 0; i < s_client_count; i++) {
        int fd = s_client_fds[i];
        if (httpd_ws_get_fd_info(s_server, fd) != HTTPD_WS_CLIENT_WEBSOCKET) {
            continue; // stale -- drop it from the registry
        }
        s_client_fds[kept++] = fd; // keep pruning the registry even while over the limit

        if (over_limit) {
            continue;
        }
        ws_send_arg_t *arg = malloc(sizeof(ws_send_arg_t));
        if (arg == NULL) {
            continue;
        }
        arg->payload = malloc(len);
        if (arg->payload == NULL) {
            free(arg);
            continue;
        }
        memcpy(arg->payload, data, len);
        arg->hd = s_server;
        arg->fd = fd;
        arg->len = len;
        if (httpd_queue_work(s_server, ws_stream_async_send, arg) != ESP_OK) {
            free(arg->payload);
            free(arg);
        } else {
            atomic_fetch_add(&s_inflight, 1);
        }
    }
    s_client_count = kept;
}

static void send_task(void *arg) {
    for (;;) {
        if (ring_buffer_used_space(s_ring) < WS_PAYLOAD_SIZE) {
            // pdMS_TO_TICKS() truncates (not rounds) at this build's
            // CONFIG_FREERTOS_HZ=100 -- see udp_stream.c's send_task for
            // the full explanation. 10ms is safely above the truncation
            // floor.
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // Strict FIFO drain, same reasoning as udp_stream.c: complete data
        // over low latency, letting delivery fall behind under sustained
        // load rather than skipping samples to catch up.
        size_t got = ring_buffer_read(s_ring, s_frame + WS_HEADER_SIZE, WS_PAYLOAD_SIZE);
        if (got == WS_PAYLOAD_SIZE && s_enabled) {
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            build_header(s_frame);
            broadcast_locked(s_frame, WS_FRAME_SIZE);
            s_seq++;
            s_sample_index += WS_FRAME_SAMPLES;
            s_status.frames_sent++;
            s_status.bytes_sent += WS_FRAME_SIZE;
            xSemaphoreGive(s_mutex);
        }
        // else: discard (streaming off, or a spurious short read --
        // shouldn't happen, SPSC).

        // Unconditional yield, even on the discard path -- see
        // udp_stream.c's send_task for why this matters (accel_capture.c
        // feeds this ring continuously regardless of enabled state).
        vTaskDelay(1);
    }
}

bool ws_stream_init(void) {
    s_ring = accel_capture_get_ws_stream_ring();
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        return false;
    }
    memset(&s_status, 0, sizeof(s_status));

    BaseType_t created = xTaskCreatePinnedToCore(send_task, "ws_stream", 4096, NULL, 6, NULL, 0);
    if (created != pdPASS) {
        printf("ws_stream: failed to create send task\n");
        return false;
    }
    return true;
}

void ws_stream_set_server(httpd_handle_t server) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_server = server;
    xSemaphoreGive(s_mutex);
}

void ws_stream_register_client(int fd) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool already = false;
    for (int i = 0; i < s_client_count; i++) {
        if (s_client_fds[i] == fd) {
            already = true;
            break;
        }
    }
    if (!already) {
        if (s_client_count < WS_STREAM_MAX_CLIENTS) {
            s_client_fds[s_client_count++] = fd;
        } else {
            // Registry full -- evict the oldest entry rather than reject
            // the new connection (it'll get pruned automatically once it
            // actually disconnects, this just bounds the registry size).
            memmove(&s_client_fds[0], &s_client_fds[1], sizeof(int) * (WS_STREAM_MAX_CLIENTS - 1));
            s_client_fds[WS_STREAM_MAX_CLIENTS - 1] = fd;
        }
    }
    xSemaphoreGive(s_mutex);
}

bool ws_stream_start(void) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_seq = 0;
    s_sample_index = 0;
    s_status.frames_sent = 0;
    s_status.bytes_sent = 0;
    xSemaphoreGive(s_mutex);

    s_enabled = true;
    return true;
}

void ws_stream_stop(void) {
    s_enabled = false;
}

void ws_stream_get_status(ws_stream_status_t *out) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_mutex);
    out->enabled = s_enabled;
    out->drops = accel_capture_get_ws_stream_ring_drops();
}
