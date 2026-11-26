#include "udp_stream.h"

#include <stdio.h>
#include <string.h>

#include "accel_capture.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "ring_buffer.h"

// Samples per packet, chosen so header+payload stays under a safe ~1472-byte
// UDP-over-Ethernet MTU budget (avoiding IP fragmentation): 16 (header) +
// 242*6 = 1468 bytes, ~110 packets/sec at the fixed 26.7kHz sample rate.
#define UDP_PACKET_SAMPLES 242
#define UDP_HEADER_SIZE 16
#define UDP_PAYLOAD_SIZE (UDP_PACKET_SAMPLES * 6)
#define UDP_PACKET_SIZE (UDP_HEADER_SIZE + UDP_PAYLOAD_SIZE)
#define UDP_MAGIC 0xAC

// Max packets drained per wake. The task wakes ~100x/sec (vTaskDelay(1) at
// CONFIG_FREERTOS_HZ=100 = 10ms/tick) while the 26.7kHz stream produces ~110
// packets/sec -- so it MUST send more than one packet per wake or it can never
// keep up. Only ~1.1 packets actually accumulate between wakes; a burst well
// above that also lets the task recover quickly after a jitter spike, while the
// cap keeps each burst short enough (a handful of non-blocking sendto()s) that
// lower-priority tasks -- IDLE, the core-0 watchdog -- always get their turn.
#define UDP_MAX_BURST 8

static ring_buffer_t *s_ring = NULL;
static int s_sock = -1;
static SemaphoreHandle_t s_mutex;
static uint8_t s_packet[UDP_PACKET_SIZE];

static volatile bool s_enabled = false; // fast-path flag, read every packet by send_task

// Everything below is protected by s_mutex.
static struct sockaddr_in s_target;
static uint32_t s_seq = 0;
static uint64_t s_sample_index = 0;
static udp_stream_status_t s_status; // .enabled/.drops filled fresh in udp_stream_get_status()

// Caller must hold s_mutex -- reads s_seq/s_sample_index/accel config directly.
static void build_header(uint8_t *buf) {
    uint8_t fs_code = (uint8_t)accel_capture_get_fs() & 0x03;
    uint8_t flags = fs_code | (accel_capture_get_lpf2() ? 0x04 : 0x00);
    uint16_t sample_count = UDP_PACKET_SAMPLES;

    buf[0] = UDP_MAGIC;
    buf[1] = flags;
    // ESP32 is little-endian and so is essentially every realistic receiver
    // (x86, ARM, Raspberry Pi) -- a plain memcpy of the native representation
    // doubles as the documented little-endian wire format.
    memcpy(buf + 2, &sample_count, sizeof(sample_count));
    memcpy(buf + 4, &s_seq, sizeof(s_seq));
    memcpy(buf + 8, &s_sample_index, sizeof(s_sample_index));
}

static void send_task(void *arg) {
    for (;;) {
        // Strict FIFO drain, deliberately: this project prefers complete data
        // over low latency for the UDP feed (unlike, say, the WS preview graph)
        // -- under sustained pressure, latency is allowed to grow rather than
        // samples being skipped to catch up. The ring (256KB, ~1.6s at the raw
        // ~160KB/s rate) absorbs real jitter; only if it fills completely does
        // accel_capture.c's write side start dropping (counted separately via
        // accel_capture_get_stream_ring_drops()).
        //
        // Drain up to a burst of already-queued packets, one per packet, with NO
        // per-packet delay. A single unconditional vTaskDelay(1) per packet
        // (10ms at CONFIG_FREERTOS_HZ=100) throttled this task to <=100
        // packets/sec -- below the ~110 packets/sec the stream generates -- so
        // the ring backed up to its full ~1.6s depth and then samples were
        // dropped at the write side. Bursting drains the ring to near-empty each
        // wake, holding latency to ~1 tick instead.
        int burst = 0;
        while (burst < UDP_MAX_BURST && ring_buffer_used_space(s_ring) >= UDP_PAYLOAD_SIZE) {
            // Read unconditionally so the ring is drained even while streaming
            // is disabled (accel_capture.c feeds it continuously regardless of
            // enabled state); only actually transmit when enabled.
            size_t got = ring_buffer_read(s_ring, s_packet + UDP_HEADER_SIZE, UDP_PAYLOAD_SIZE);
            if (got != UDP_PAYLOAD_SIZE) {
                break; // spurious short read -- shouldn't happen (SPSC)
            }
            burst++;
            if (!s_enabled) {
                continue; // drained/discarded above; nothing to send
            }

            xSemaphoreTake(s_mutex, portMAX_DELAY);
            build_header(s_packet);
            struct sockaddr_in target = s_target;
            xSemaphoreGive(s_mutex);

            int sent = sendto(s_sock, s_packet, UDP_PACKET_SIZE, 0, (struct sockaddr *)&target,
                               sizeof(target));

            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_seq++;
            s_sample_index += UDP_PACKET_SAMPLES;
            if (sent > 0) {
                s_status.packets_sent++;
                s_status.bytes_sent += (uint64_t)sent;
            }
            xSemaphoreGive(s_mutex);
        }

        // Yield once per wake, whether we emptied the ring (nothing to do until
        // ~a tick of samples accumulates) or hit the burst cap with more still
        // waiting (come straight back after a short breather). This also keeps
        // the discard path (streaming off) from busy-spinning and starving the
        // core-0 IDLE task / tripping the watchdog. Pass a raw tick count (1),
        // not pdMS_TO_TICKS(1): the latter truncates to 0 ticks at
        // CONFIG_FREERTOS_HZ=100 and vTaskDelay(0) doesn't reliably yield to a
        // lower-priority task like IDLE.
        vTaskDelay(1);
    }
}

bool udp_stream_init(void) {
    s_ring = accel_capture_get_stream_ring();
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        return false;
    }
    memset(&s_status, 0, sizeof(s_status));

    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        printf("udp_stream: failed to create socket\n");
        return false;
    }

    BaseType_t created = xTaskCreatePinnedToCore(send_task, "udp_stream", 4096, NULL, 6, NULL, 0);
    if (created != pdPASS) {
        printf("udp_stream: failed to create send task\n");
        return false;
    }
    return true;
}

bool udp_stream_start(const char *ip, uint16_t port) {
    struct sockaddr_in target = {0};
    target.sin_family = AF_INET;
    target.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &target.sin_addr) != 1) {
        return false;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_target = target;
    s_seq = 0;
    s_sample_index = 0;
    s_status.packets_sent = 0;
    s_status.bytes_sent = 0;
    strncpy(s_status.target_ip, ip, sizeof(s_status.target_ip) - 1);
    s_status.target_ip[sizeof(s_status.target_ip) - 1] = '\0';
    s_status.target_port = port;
    xSemaphoreGive(s_mutex);

    s_enabled = true; // set last, after the target is fully written
    return true;
}

void udp_stream_stop(void) {
    s_enabled = false;
}

void udp_stream_get_status(udp_stream_status_t *out) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_mutex);
    out->enabled = s_enabled;
    out->drops = accel_capture_get_stream_ring_drops();
}
