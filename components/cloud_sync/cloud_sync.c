#include "cloud_sync.h"

#include "config_store.h"
#include "telemetry.h"
#include "wifi_manager.h"

#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "cloud";

#define CLOUD_NVS_NS          "cloud"
#define CLOUD_NVS_OFFSET_KEY  "csv_offset"
#define CLOUD_URL_MAX         256
#define CLOUD_TOKEN_MAX       192
#define CLOUD_PAYLOAD_MAX     4096
#define CLOUD_LINE_MAX        256

static TaskHandle_t s_task;

static void cloud_sync_task(void *arg);
static esp_err_t upload_pending_rows(void);

static bool starts_with(const char *s, const char *prefix)
{
    return s && prefix && strncmp(s, prefix, strlen(prefix)) == 0;
}

static uint64_t load_csv_offset(void)
{
    uint64_t offset = 0;
    nvs_handle_t h;
    if (nvs_open(CLOUD_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u64(h, CLOUD_NVS_OFFSET_KEY, &offset);
        nvs_close(h);
    }
    return offset;
}

static void save_csv_offset(uint64_t offset)
{
    nvs_handle_t h;
    if (nvs_open(CLOUD_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u64(h, CLOUD_NVS_OFFSET_KEY, offset);
        nvs_commit(h);
        nvs_close(h);
    }
}

static bool append_raw(char *buf, size_t buf_sz, size_t *pos, const char *text)
{
    size_t len = strlen(text);
    if (*pos + len >= buf_sz) {
        return false;
    }
    memcpy(buf + *pos, text, len);
    *pos += len;
    buf[*pos] = '\0';
    return true;
}

static bool append_json_string(char *buf, size_t buf_sz, size_t *pos, const char *text)
{
    if (!append_raw(buf, buf_sz, pos, "\"")) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)text; p && *p; p++) {
        char esc[8];
        switch (*p) {
        case '\\':
        case '"':
            if (*pos + 2 >= buf_sz) return false;
            buf[(*pos)++] = '\\';
            buf[(*pos)++] = (char)*p;
            break;
        case '\b':
            if (!append_raw(buf, buf_sz, pos, "\\b")) return false;
            break;
        case '\f':
            if (!append_raw(buf, buf_sz, pos, "\\f")) return false;
            break;
        case '\n':
            if (!append_raw(buf, buf_sz, pos, "\\n")) return false;
            break;
        case '\r':
            if (!append_raw(buf, buf_sz, pos, "\\r")) return false;
            break;
        case '\t':
            if (!append_raw(buf, buf_sz, pos, "\\t")) return false;
            break;
        default:
            if (*p < 0x20) {
                snprintf(esc, sizeof(esc), "\\u%04x", *p);
                if (!append_raw(buf, buf_sz, pos, esc)) return false;
            } else {
                if (*pos + 1 >= buf_sz) return false;
                buf[(*pos)++] = (char)*p;
            }
            break;
        }
        buf[*pos] = '\0';
    }
    return append_raw(buf, buf_sz, pos, "\"");
}

static bool trim_line(char *line)
{
    size_t len = strlen(line);
    if (len == 0 || line[len - 1] != '\n') {
        return false;
    }
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        line[--len] = '\0';
    }
    return len > 0;
}

static bool build_payload(FILE *f, char *payload, size_t payload_sz,
                          int *rows_out, uint64_t *new_offset_out)
{
    char model[64] = {0};
    char serial[64] = {0};
    char line[CLOUD_LINE_MAX];
    size_t pos = 0;
    int rows = 0;
    uint64_t last_complete_offset = (uint64_t)ftell(f);

    config_get_device_model(model, sizeof(model));
    config_get_serial_number(serial, sizeof(serial));
    if (serial[0] == '\0') {
        snprintf(serial, sizeof(serial), "unknown");
    }

    payload[0] = '\0';
    if (!append_raw(payload, payload_sz, &pos, "{\"schema\":\"esp-idms-csv-v1\",\"device_model\":") ||
        !append_json_string(payload, payload_sz, &pos, model) ||
        !append_raw(payload, payload_sz, &pos, ",\"serial_number\":") ||
        !append_json_string(payload, payload_sz, &pos, serial) ||
        !append_raw(payload, payload_sz, &pos, ",\"rows\":[")) {
        return false;
    }

    while (rows < CONFIG_IDMS_CLOUD_UPLOAD_MAX_ROWS && fgets(line, sizeof(line), f)) {
        uint64_t after_line = (uint64_t)ftell(f);
        if (!trim_line(line)) {
            break;
        }

        size_t saved_pos = pos;
        if (rows > 0 && !append_raw(payload, payload_sz, &pos, ",")) {
            pos = saved_pos;
            break;
        }
        if (!append_json_string(payload, payload_sz, &pos, line)) {
            pos = saved_pos;
            break;
        }

        rows++;
        last_complete_offset = after_line;
    }

    if (!append_raw(payload, payload_sz, &pos, "]}")) {
        return false;
    }

    *rows_out = rows;
    *new_offset_out = last_complete_offset;
    return true;
}

static esp_err_t post_payload(const char *url, const char *token, const char *payload)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 10000,
        .buffer_size = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    if (token && token[0]) {
        char auth[CLOUD_TOKEN_MAX + 16];
        snprintf(auth, sizeof(auth), "Bearer %s", token);
        esp_http_client_set_header(client, "Authorization", auth);
    }
    esp_http_client_set_post_field(client, payload, (int)strlen(payload));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Cloud upload failed: %s", esp_err_to_name(err));
        return err;
    }
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "Cloud upload rejected: HTTP %d", status);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static uint64_t skip_header(FILE *f)
{
    char line[CLOUD_LINE_MAX];
    rewind(f);
    if (!fgets(line, sizeof(line), f)) {
        return 0;
    }
    return (uint64_t)ftell(f);
}

static esp_err_t upload_pending_rows(void)
{
    char url[CLOUD_URL_MAX] = {0};
    char token[CLOUD_TOKEN_MAX] = {0};
    char *payload = NULL;
    FILE *f = NULL;
    esp_err_t result = ESP_OK;
    bool csv_locked = false;

    ESP_RETURN_ON_ERROR(config_get_cloud_url(url, sizeof(url)), TAG, "get cloud url");
    if (url[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    if (!starts_with(url, "https://")
#if CONFIG_IDMS_CLOUD_ALLOW_INSECURE_HTTP
        && !starts_with(url, "http://")
#endif
        ) {
        ESP_LOGW(TAG, "Cloud sync disabled: invalid URL '%s'", url);
        return ESP_ERR_INVALID_ARG;
    }
    if (!wifi_manager_is_connected()) {
        return ESP_ERR_INVALID_STATE;
    }

    config_get_cloud_token(token, sizeof(token));

    result = telemetry_csv_lock(15000);
    if (result != ESP_OK) {
        return result;
    }
    csv_locked = true;

    struct stat st;
    const char *path = telemetry_csv_path();
    if (stat(path, &st) != 0 || st.st_size <= 0) {
        result = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }

    f = fopen(path, "r");
    if (!f) {
        result = ESP_FAIL;
        goto cleanup;
    }

    uint64_t offset = load_csv_offset();
    if (offset == (uint64_t)st.st_size) {
        result = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }
    if (offset == 0 || offset > (uint64_t)st.st_size) {
        offset = skip_header(f);
        save_csv_offset(offset);
    } else {
        fseek(f, (long)offset, SEEK_SET);
    }

    payload = calloc(1, CLOUD_PAYLOAD_MAX);
    if (!payload) {
        result = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    int rows = 0;
    uint64_t new_offset = offset;
    bool built = build_payload(f, payload, CLOUD_PAYLOAD_MAX, &rows, &new_offset);
    fclose(f);
    f = NULL;

    if (!built) {
        result = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    if (rows == 0) {
        result = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }

    result = post_payload(url, token, payload);
    if (result == ESP_OK) {
        save_csv_offset(new_offset);
        ESP_LOGI(TAG, "Uploaded %d telemetry row(s), csv_offset=%llu",
                 rows, (unsigned long long)new_offset);
    }

cleanup:
    if (f) {
        fclose(f);
    }
    free(payload);
    if (csv_locked) {
        telemetry_csv_unlock();
    }
    return result;
}

esp_err_t cloud_sync_init(void)
{
    if (s_task) {
        return ESP_OK;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(cloud_sync_task, "cloud_sync",
                                            8192, NULL, 3, &s_task, tskNO_AFFINITY);
    if (ok != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Cloud sync task started (interval %u s)",
             (unsigned)CONFIG_IDMS_CLOUD_UPLOAD_INTERVAL_S);
    return ESP_OK;
}

static void cloud_sync_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(15000));

    for (;;) {
        esp_err_t err = upload_pending_rows();
        if (err == ESP_ERR_INVALID_STATE) {
            ESP_LOGD(TAG, "Cloud sync disabled: no URL configured");
        } else if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_INVALID_STATE) {
            ESP_LOGD(TAG, "Cloud sync waiting: %s", esp_err_to_name(err));
        } else if (err != ESP_OK) {
            ESP_LOGW(TAG, "Cloud sync attempt failed: %s", esp_err_to_name(err));
        }

        vTaskDelay(pdMS_TO_TICKS(CONFIG_IDMS_CLOUD_UPLOAD_INTERVAL_S * 1000));
    }
}
