#include "ota.h"
#include "telegram.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_system.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "ota";

#define OTA_RECV_BUFSZ 4096
#define OTA_NVS_NAMESPACE "ota_state"

/* Version string built at compile time */
#ifndef PROJECT_VER
#define PROJECT_VER "unknown"
#endif

static char s_version[96];
static char s_status[32];
static char s_partition[16];
static volatile bool s_telegram_trigger = false;
static volatile bool s_update_in_progress = false;

/* ------------------------------------------------------------------ */
/*  Basic-auth helper (RFC 7617 — simple Base64, no constant-time)   */
/* ------------------------------------------------------------------ */

static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_encode(char *out, size_t out_sz, const uint8_t *in, size_t in_len)
{
    if (out_sz < 4 * ((in_len + 2) / 3) + 1) {
        return -1;
    }
    size_t i = 0, j = 0;
    while (i < in_len) {
        uint32_t octet_a = i < in_len ? in[i++] : 0;
        uint32_t octet_b = i < in_len ? in[i++] : 0;
        uint32_t octet_c = i < in_len ? in[i++] : 0;
        uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;
        out[j++] = b64_table[(triple >> 18) & 0x3F];
        out[j++] = b64_table[(triple >> 12) & 0x3F];
        out[j++] = (i - 1 < in_len) ? b64_table[(triple >> 6) & 0x3F] : '=';
        out[j++] = (i - 2 < in_len) ? b64_table[triple & 0x3F] : '=';
    }
    out[j] = '\0';
    return (int)j;
}

static bool check_basic_auth(httpd_req_t *req)
{
    char auth_header[256];
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth_header, sizeof(auth_header)) != ESP_OK) {
        return false;
    }
    if (strncmp(auth_header, "Basic ", 6) != 0) {
        return false;
    }
    const char *creds = auth_header + 6;
    char expected_creds[256];
    snprintf(expected_creds, sizeof(expected_creds), "%s:%s",
             CONFIG_IDMS_OTA_HTTP_AUTH_USER, CONFIG_IDMS_OTA_HTTP_AUTH_PASS);
    char expected_b64[384];
    int n = b64_encode(expected_b64, sizeof(expected_b64),
                       (const uint8_t *)expected_creds, strlen(expected_creds));
    if (n < 0) {
        return false;
    }
    return strncmp(creds, expected_b64, (size_t)n) == 0;
}

/* ------------------------------------------------------------------ */
/*  HTTP OTA upload handler                                            */
/* ------------------------------------------------------------------ */

static esp_ota_handle_t s_ota_handle = 0;
static const esp_partition_t *s_update_part = NULL;

/* Simple multipart parser — finds the file part boundary and streams it to OTA */

static bool is_end_boundary(const char *line, const char *boundary)
{
    if (strncmp(line, boundary, strlen(boundary)) != 0) {
        return false;
    }
    const char *after = line + strlen(boundary);
    return (strncmp(after, "--", 2) == 0);
}

static esp_err_t ota_upload_handler(httpd_req_t *req)
{
    if (!check_basic_auth(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "Authentication required");
        return ESP_OK;
    }

    if (s_update_in_progress) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "OTA update already in progress");
        return ESP_OK;
    }

    /* Get Content-Type to extract boundary */
    char content_type[256];
    if (httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "Missing Content-Type");
        return ESP_OK;
    }

    char *boundary_start = strstr(content_type, "boundary=");
    if (!boundary_start) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "Missing boundary in Content-Type");
        return ESP_OK;
    }
    boundary_start += 9; /* skip "boundary=" */

    char boundary[128];
    snprintf(boundary, sizeof(boundary), "--%s", boundary_start);
    /* Strip trailing whitespace / quotes */
    size_t blen = strlen(boundary);
    while (blen > 0 && (boundary[blen - 1] == '\r' || boundary[blen - 1] == '\n' || boundary[blen - 1] == '\"')) {
        boundary[--blen] = '\0';
    }

    ESP_LOGI(TAG, "OTA upload started, boundary: %s", boundary);

    /* Determine target partition */
    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
    if (!update) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "No OTA partition available");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Writing to partition %s at offset 0x%lx", update->label, (unsigned long)update->address);

    esp_err_t err = esp_ota_begin(update, OTA_WITH_SEQUENTIAL_WRITES, &s_ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "Failed to begin OTA");
        return ESP_OK;
    }

    s_update_part = update;
    s_update_in_progress = true;

    char *buf = malloc(OTA_RECV_BUFSZ);
    if (!buf) {
        esp_ota_abort(s_ota_handle);
        s_ota_handle = 0;
        s_update_in_progress = false;
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    size_t total = 0;
    int recv_len;
    bool in_file_data = false;
    bool skip_headers = true;
    char line_buf[256];
    size_t line_pos = 0;
    bool upload_ok = true;

    while ((recv_len = httpd_req_recv(req, buf, OTA_RECV_BUFSZ)) > 0) {
        int buf_offset = 0;

        while (buf_offset < recv_len) {
            if (!in_file_data) {
                /* Parse lines looking for multipart boundaries */
                while (buf_offset < recv_len && line_pos < sizeof(line_buf) - 1) {
                    char c = buf[buf_offset++];
                    line_buf[line_pos++] = c;
                    if (c == '\n') {
                        line_buf[line_pos] = '\0';

                        if (strncmp(line_buf, boundary, strlen(boundary)) == 0) {
                            if (is_end_boundary(line_buf, boundary)) {
                                /* End of multipart — done */
                                goto upload_complete;
                            }
                            /* Start of a new part — reset */
                            in_file_data = false;
                            skip_headers = true;
                            line_pos = 0;
                        } else if (skip_headers) {
                            /* Empty line = end of headers for this part */
                            if (line_pos <= 2) {
                                skip_headers = false;
                                in_file_data = true;
                                ESP_LOGI(TAG, "Start of file data");
                            }
                            line_pos = 0;
                        }
                    }
                }
            } else {
                /* Inside the file part — stream data to OTA until next boundary */
                int write_start = buf_offset;
                size_t remaining = recv_len - buf_offset;

                /* Search for boundary in remaining data */
                char *found = memmem(buf + buf_offset, remaining, boundary, strlen(boundary));
                if (found) {
                    size_t before = (size_t)(found - buf);
                    if (before > (size_t)write_start) {
                        size_t chunk = before - write_start;
                        err = esp_ota_write(s_ota_handle, buf + write_start, chunk);
                        if (err != ESP_OK) {
                            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
                            upload_ok = false;
                            goto upload_complete;
                        }
                        total += chunk;
                    }
                    /* Resume boundary parsing from the boundary */
                    in_file_data = false;
                    skip_headers = true;
                    line_pos = 0;
                    buf_offset = (int)before;
                } else {
                    /* No boundary — write entire remaining chunk */
                    err = esp_ota_write(s_ota_handle, buf + write_start, remaining);
                    if (err != ESP_OK) {
                        ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
                        upload_ok = false;
                        goto upload_complete;
                    }
                    total += remaining;
                    buf_offset = recv_len;
                }
            }
        }
    }

upload_complete:
    if (recv_len < 0 && upload_ok) {
        /* Connection dropped before end — treat as failure */
        upload_ok = false;
    }

    if (!upload_ok) {
        esp_ota_abort(s_ota_handle);
        s_ota_handle = 0;
        s_update_in_progress = false;
        free(buf);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain");
        char msg[80];
        snprintf(msg, sizeof(msg), "Upload failed (received %zu bytes)", total);
        httpd_resp_sendstr(req, msg);
        return ESP_OK;
    }

    /* Finalize OTA */
    err = esp_ota_end(s_ota_handle);
    s_ota_handle = 0;
    s_update_in_progress = false;
    free(buf);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "OTA completion failed (bad image?)");
        return ESP_OK;
    }

    err = esp_ota_set_boot_partition(s_update_part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "Failed to set boot partition");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "OTA complete: %zu bytes written, rebooting...", total);

    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_type(req, "text/plain");
    char msg[128];
    snprintf(msg, sizeof(msg), "OTA successful! Rebooting into new firmware (%zu bytes).\n", total);
    httpd_resp_sendstr(req, msg);

    /* Notify technicians via Telegram */
    telegram_broadcast_text("🔄 OTA update successful. Rebooting into new firmware.");

    /* Delay so the HTTP response is sent, then reboot */
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();

    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  HTTP status / info endpoint                                        */
/* ------------------------------------------------------------------ */

static esp_err_t ota_info_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");

    char body[512];
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_app_desc_t app_desc;
    esp_ota_get_partition_description(running, &app_desc);

    snprintf(body, sizeof(body),
             "{"
             "\"version\":\"%s\","
             "\"partition\":\"%s\","
             "\"status\":\"%s\","
             "\"idf_version\":\"%s\","
             "\"chip\":\"%s\","
             "\"compile_time\":\"%s %s\""
             "}",
             ota_get_version(),
             running->label,
             ota_get_status(),
             app_desc.version,
#if CONFIG_IDF_TARGET_ESP32S3
             "ESP32-S3",
#elif CONFIG_IDF_TARGET_ESP32
             "ESP32",
#else
             "ESP32-variant",
#endif
             app_desc.date, app_desc.time);

    httpd_resp_sendstr(req, body);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Simple HTML upload page                                            */
/* ------------------------------------------------------------------ */

static esp_err_t ota_page_handler(httpd_req_t *req)
{
    const char *page =
        "<!DOCTYPE html>"
        "<html><head><meta charset=\"utf-8\"><title>ESP-IDMS OTA Update</title>"
        "<style>"
        "body{font-family:system-ui,sans-serif;max-width:600px;margin:40px auto;padding:0 20px;background:#1a1a2e;color:#eee}"
        "h1{color:#00d4ff}input[type=file]{margin:20px 0}input[type=submit]{"
        "background:#00d4ff;color:#000;border:none;padding:10px 24px;font-size:16px;cursor:pointer;border-radius:4px}"
        "input[type=submit]:hover{background:#00b8d4}"
        ".warn{color:#ff6b6b;font-size:14px;margin-top:16px}"
        "</style></head><body>"
        "<h1>ESP-IDMS Firmware Update</h1>"
        "<p>Current version: <strong>" PROJECT_VER "</strong></p>"
        "<form method=\"POST\" enctype=\"multipart/form-data\">"
        "<label>Firmware binary (.bin):</label><br>"
        "<input type=\"file\" name=\"firmware\" accept=\".bin\" required><br>"
        "<input type=\"submit\" value=\"Upload &amp; Update\">"
        "</form>"
        "<p class=\"warn\">⚠ The device will reboot after a successful update.</p>"
        "</body></html>";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, page);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  HTTP server startup                                                */
/* ------------------------------------------------------------------ */

static esp_err_t start_ota_http_server(uint16_t port)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    config.max_uri_handlers = 8;
    config.recv_wait_timeout = 30;
    config.send_wait_timeout = 30;
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;

    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    /* GET / → upload page */
    httpd_uri_t page_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = ota_page_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &page_uri);

    /* POST / → firmware upload */
    httpd_uri_t upload_uri = {
        .uri = "/",
        .method = HTTP_POST,
        .handler = ota_upload_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &upload_uri);

    /* GET /info → JSON status */
    httpd_uri_t info_uri = {
        .uri = "/info",
        .method = HTTP_GET,
        .handler = ota_info_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &info_uri);

    ESP_LOGI(TAG, "OTA HTTP server started on port %u", (unsigned)port);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Boot validation / rollback check                                   */
/* ------------------------------------------------------------------ */

static void check_boot_state(void)
{
    esp_ota_img_states_t state;
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_err_t err = esp_ota_get_state_partition(running, &state);

    if (err == ESP_OK) {
        if (state == ESP_OTA_IMG_PENDING_VERIFY) {
            /* This is the first boot after an OTA — mark as valid */
            ESP_LOGI(TAG, "First boot after OTA on %s — marking as valid", running->label);
            esp_ota_mark_app_valid_cancel_rollback();
            strncpy(s_status, "ready", sizeof(s_status) - 1);
        } else if (state == ESP_OTA_IMG_ABORTED) {
            strncpy(s_status, "rollback occurred", sizeof(s_status) - 1);
            ESP_LOGW(TAG, "Previous OTA was aborted — rolled back to %s", running->label);
        } else {
            strncpy(s_status, "ready", sizeof(s_status) - 1);
        }
    } else {
        strncpy(s_status, "ready", sizeof(s_status) - 1);
    }

    strncpy(s_partition, running->label, sizeof(s_partition) - 1);
}

/* ------------------------------------------------------------------ */
/*  Version string construction                                        */
/* ------------------------------------------------------------------ */

static void build_version_string(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_app_desc_t app_desc;
    if (esp_ota_get_partition_description(running, &app_desc) == ESP_OK) {
        snprintf(s_version, sizeof(s_version), "v%s-%s-%s_%s",
                 app_desc.version,
#if CONFIG_IDF_TARGET_ESP32S3
                 "s3",
#elif CONFIG_IDF_TARGET_ESP32
                 "esp32",
#else
                 "esp32x",
#endif
                 app_desc.date, app_desc.time);
    } else {
        snprintf(s_version, sizeof(s_version), "v%s-unknown", PROJECT_VER);
    }
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

esp_err_t ota_init(void)
{
    build_version_string();
    check_boot_state();

    ESP_LOGI(TAG, "OTA subsystem initialized. Version: %s, partition: %s, status: %s",
             s_version, s_partition, s_status);

#if CONFIG_IDMS_OTA_ENABLE
    esp_err_t err = start_ota_http_server(CONFIG_IDMS_OTA_HTTP_PORT);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start OTA HTTP server: %s", esp_err_to_name(err));
        return err;
    }
#endif

    return ESP_OK;
}

const char *ota_get_version(void)
{
    return s_version;
}

const char *ota_get_status(void)
{
    if (s_update_in_progress) {
        return "updating";
    }
    return s_status;
}

const char *ota_get_partition(void)
{
    return s_partition;
}

void ota_mark_app_valid(void)
{
    esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGI(TAG, "App marked valid — rollback cancelled");
}

void ota_trigger_from_telegram(void)
{
    s_telegram_trigger = true;
    ESP_LOGI(TAG, "OTA update requested via Telegram");
}

bool ota_is_requested(void)
{
    if (s_telegram_trigger) {
        s_telegram_trigger = false;
        return true;
    }
    return false;
}
