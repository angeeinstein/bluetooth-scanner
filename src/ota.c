#include "ota.h"

#include <string.h>

#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ota_token.h"
#include "recorder.h"

// The device only accepts an uploaded image that contains this compiled-in
// secret somewhere in its bytes -- proof it was built from this source. See
// ota_token.h. Empty token => check disabled.
static const char s_ota_token[] = OTA_UPDATE_TOKEN;
#define OTA_TOKEN_LEN (sizeof(s_ota_token) - 1)
#define OTA_TOKEN_MAX 96
_Static_assert(OTA_TOKEN_LEN < OTA_TOKEN_MAX, "OTA update token is too long");

static bool s_token_found = false;
static uint8_t s_token_tail[OTA_TOKEN_MAX]; // trailing bytes of the last chunk, for cross-chunk matches
static size_t s_token_tail_len = 0;

static bool find_bytes(const uint8_t *hay, size_t hlen, const uint8_t *needle, size_t nlen) {
    if (nlen == 0 || hlen < nlen) {
        return false;
    }
    for (size_t i = 0; i + nlen <= hlen; i++) {
        if (hay[i] == needle[0] && memcmp(hay + i, needle, nlen) == 0) {
            return true;
        }
    }
    return false;
}

// Streams the image past a substring matcher, catching the token even when it
// straddles two chunks (via the retained tail of the previous chunk).
static void scan_for_token(const uint8_t *data, size_t len) {
    if (OTA_TOKEN_LEN == 0 || s_token_found) {
        return;
    }
    const uint8_t *tok = (const uint8_t *)s_ota_token;
    size_t keep = OTA_TOKEN_LEN - 1; // bytes to carry across a chunk boundary

    if (s_token_tail_len > 0) {
        uint8_t boundary[OTA_TOKEN_MAX * 2];
        size_t take = len < keep ? len : keep;
        memcpy(boundary, s_token_tail, s_token_tail_len);
        memcpy(boundary + s_token_tail_len, data, take);
        if (find_bytes(boundary, s_token_tail_len + take, tok, OTA_TOKEN_LEN)) {
            s_token_found = true;
            return;
        }
    }
    if (find_bytes(data, len, tok, OTA_TOKEN_LEN)) {
        s_token_found = true;
        return;
    }
    // Retain the last `keep` bytes (of tail+data) for the next chunk's boundary.
    if (len >= keep) {
        memcpy(s_token_tail, data + len - keep, keep);
        s_token_tail_len = keep;
    } else {
        uint8_t tmp[OTA_TOKEN_MAX * 2];
        size_t combined = s_token_tail_len + len;
        memcpy(tmp, s_token_tail, s_token_tail_len);
        memcpy(tmp + s_token_tail_len, data, len);
        size_t start = combined > keep ? combined - keep : 0;
        s_token_tail_len = combined - start;
        memcpy(s_token_tail, tmp + start, s_token_tail_len);
    }
}

// All OTA calls and the status read happen on the single httpd task (the upload
// handler and the status-JSON builder are both HTTP handlers), so plain statics
// need no lock. ota_mark_valid() runs once at boot before any of that.
static ota_state_t s_state = OTA_IDLE;
static esp_ota_handle_t s_handle = 0;
static const esp_partition_t *s_target = NULL;
static uint32_t s_written = 0;
static uint32_t s_total = 0;
static char s_error[64] = "";

static void set_error(const char *msg) {
    strncpy(s_error, msg, sizeof(s_error) - 1);
    s_error[sizeof(s_error) - 1] = '\0';
}

bool ota_begin(uint32_t total_bytes) {
    if (s_state == OTA_WRITING) {
        set_error("an update is already in progress");
        return false;
    }
    if (recorder_is_recording()) {
        set_error("stop the recording first");
        s_state = OTA_FAILED;
        return false;
    }
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    if (next == NULL) {
        set_error("no OTA partition -- flash the OTA layout over USB first");
        s_state = OTA_FAILED;
        return false;
    }
    esp_err_t err = esp_ota_begin(next, total_bytes > 0 ? total_bytes : OTA_SIZE_UNKNOWN, &s_handle);
    if (err != ESP_OK) {
        set_error("esp_ota_begin failed");
        s_state = OTA_FAILED;
        return false;
    }
    s_target = next;
    s_written = 0;
    s_total = total_bytes;
    s_error[0] = '\0';
    s_token_found = false;
    s_token_tail_len = 0;
    s_state = OTA_WRITING;
    return true;
}

bool ota_write(const void *data, size_t len) {
    if (s_state != OTA_WRITING) {
        return false;
    }
    esp_err_t err = esp_ota_write(s_handle, data, len);
    if (err != ESP_OK) {
        esp_ota_abort(s_handle); // consumes the handle
        set_error("flash write failed");
        s_state = OTA_FAILED;
        return false;
    }
    scan_for_token((const uint8_t *)data, len);
    s_written += (uint32_t)len;
    return true;
}

bool ota_end(void) {
    if (s_state != OTA_WRITING) {
        return false;
    }
    // Authenticity gate: the image must carry our compiled-in token (see
    // ota_token.h) -- rejects a valid-but-unrelated firmware someone else built.
    if (OTA_TOKEN_LEN > 0 && !s_token_found) {
        esp_ota_abort(s_handle); // discard -- never activate an unrecognized image
        set_error("unrecognized firmware -- not a VibraShock build (missing update token)");
        s_state = OTA_FAILED;
        return false;
    }
    esp_err_t err = esp_ota_end(s_handle); // validates magic/size/SHA-256, consumes the handle
    if (err != ESP_OK) {
        set_error(err == ESP_ERR_OTA_VALIDATE_FAILED ? "image failed validation (not a valid build?)"
                                                      : "esp_ota_end failed");
        s_state = OTA_FAILED;
        return false;
    }
    err = esp_ota_set_boot_partition(s_target);
    if (err != ESP_OK) {
        set_error("could not set the boot partition");
        s_state = OTA_FAILED;
        return false;
    }
    s_state = OTA_SUCCESS;
    return true;
}

void ota_abort(void) {
    if (s_state == OTA_WRITING) {
        esp_ota_abort(s_handle);
        s_state = OTA_FAILED;
        if (s_error[0] == '\0') {
            set_error("update aborted");
        }
    }
}

void ota_get_status(ota_status_t *out) {
    memset(out, 0, sizeof(*out));
    out->state = s_state;
    out->bytes_written = s_written;
    out->total_bytes = s_total;
    strncpy(out->error, s_error, sizeof(out->error) - 1);
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (run != NULL) {
        strncpy(out->running_partition, run->label, sizeof(out->running_partition) - 1);
    }
    out->available = esp_ota_get_next_update_partition(NULL) != NULL;
}

void ota_mark_valid(void) {
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (run != NULL && esp_ota_get_state_partition(run, &st) == ESP_OK &&
        st == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
    }
}

static void reboot_task(void *arg) {
    vTaskDelay(pdMS_TO_TICKS(1500)); // let the HTTP success response flush first
    esp_restart();
}

void ota_schedule_reboot(void) {
    xTaskCreate(reboot_task, "ota_reboot", 2048, NULL, 5, NULL);
}
