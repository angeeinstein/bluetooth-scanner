#include "sync_trigger.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "pins.h"

#define EDGE_RING 128 // recent edges kept for the /api/sync/edges readout

// One ISR->task item: the edge number and the esp_timer instant it fired at.
typedef struct {
    uint32_t n;
    int64_t esp_us;
} isr_evt_t;

static volatile uint32_t s_edge_count = 0; // written by the ISR (and reset)
static sync_edge_t s_ring[EDGE_RING];
static uint32_t s_ring_head = 0; // next write slot
static QueueHandle_t s_edge_q = NULL;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static void IRAM_ATTR edge_isr(void *arg) {
    (void)arg;
    isr_evt_t e = {.n = ++s_edge_count, .esp_us = esp_timer_get_time()};
    BaseType_t hpw = pdFALSE;
    xQueueSendFromISR(s_edge_q, &e, &hpw);
    if (hpw) {
        portYIELD_FROM_ISR();
    }
}

static void edge_task(void *arg) {
    (void)arg;
    isr_evt_t e;
    for (;;) {
        if (xQueueReceive(s_edge_q, &e, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        // The TSF can't be read safely from the ISR, so we read it here and undo
        // the small ISR->task delay with the fast esp_timer:
        //   tsf_at_edge = tsf_now - (esp_now - esp_at_edge)
        // The interval is only the wakeup latency (tens of us), over which the
        // esp/TSF clocks drift by well under a microsecond -- negligible.
        int64_t tsf_now = esp_wifi_get_tsf_time(WIFI_IF_STA);
        int64_t esp_now = esp_timer_get_time();
        int64_t tsf_at_edge = (tsf_now > 0) ? (tsf_now - (esp_now - e.esp_us)) : 0;

        taskENTER_CRITICAL(&s_mux);
        s_ring[s_ring_head].n = e.n;
        s_ring[s_ring_head].tsf_us = tsf_at_edge;
        s_ring_head = (s_ring_head + 1) % EDGE_RING;
        taskEXIT_CRITICAL(&s_mux);
    }
}

void sync_trigger_start(void) {
    s_edge_q = xQueueCreate(64, sizeof(isr_evt_t));
    if (s_edge_q == NULL) {
        printf("sync_trigger: queue alloc failed -- disabled\n");
        return;
    }
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_SYNC_TRIG,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE, // idle low; only a driven rising edge fires
        .intr_type = GPIO_INTR_POSEDGE,
    };
    gpio_config(&io);
    // The GPIO ISR service is normally already installed (accel_capture's FIFO
    // interrupt) -- ESP_ERR_INVALID_STATE just means "already there", which is fine.
    gpio_install_isr_service(0);
    gpio_isr_handler_add(PIN_SYNC_TRIG, edge_isr, NULL);
    xTaskCreate(edge_task, "sync_edge", 3072, NULL, 10, NULL);
    printf("sync_trigger: external sync input armed on IO%d\n", PIN_SYNC_TRIG);
}

void sync_trigger_reset(void) {
    taskENTER_CRITICAL(&s_mux);
    s_edge_count = 0;
    s_ring_head = 0;
    memset(s_ring, 0, sizeof(s_ring));
    taskEXIT_CRITICAL(&s_mux);
    if (s_edge_q != NULL) {
        xQueueReset(s_edge_q);
    }
}

uint32_t sync_trigger_get_count(void) {
    return s_edge_count;
}

int sync_trigger_get_edges(sync_edge_t *out, int max) {
    int n = 0;
    taskENTER_CRITICAL(&s_mux);
    for (int i = 0; i < EDGE_RING && n < max; i++) {
        if (s_ring[i].n != 0) {
            out[n++] = s_ring[i];
        }
    }
    taskEXIT_CRITICAL(&s_mux);
    return n;
}
