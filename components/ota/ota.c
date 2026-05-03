#include "ota.h"
#include "telegram.h"
#include "config_store.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_system.h"
#include "esp_http_server.h"
#include "esp_random.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "mbedtls/base64.h"
#include "mbedtls/sha256.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "ota";

#define OTA_RECV_BUFSZ 4096
#define TOKEN_EXPIRY_S 300
#define AUTH_FAIL_LIMIT 5
#define AUTH_LOCKOUT_S 60

#ifndef PROJECT_VER
#define PROJECT_VER "unknown"
#endif

static char s_version[96];
static char s_status[32];
static char s_partition[16];
static esp_ota_handle_t s_ota_handle = 0;
static volatile bool s_update_in_progress = false;

static char s_cached_user[64] = {0};
static char s_cached_pass[64] = {0};
static bool s_creds_cached = false;

static struct {
    char token[17];
    uint32_t created_tick;
} s_ota_token;

static struct {
    int fail_count;
    uint32_t locked_until_tick;
} s_auth_rate;

static void load_cached_creds(void)
{
    if (s_creds_cached) return;
    config_get_ota_user(s_cached_user, sizeof(s_cached_user));
    config_get_ota_pass(s_cached_pass, sizeof(s_cached_pass));
    s_creds_cached = true;
}

static int b64_encode(char *out, size_t out_sz, const uint8_t *in, size_t in_len)
{
    if (!out || out_sz == 0) {
        return -1;
    }

    size_t olen = 0;
    int ret = mbedtls_base64_encode((unsigned char *)out, out_sz, &olen, in, in_len);
    if (ret != 0 || olen >= out_sz) {
        return -1;
    }

    out[olen] = '\0';
    return (int)olen;
}

static bool ct_memcmp(const void *a, const void *b, size_t len)
{
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    volatile uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) {
        diff |= pa[i] ^ pb[i];
    }
    return diff == 0;
}

static bool is_rate_limited(void)
{
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS / 1000;
    if (s_auth_rate.locked_until_tick > 0 && now < s_auth_rate.locked_until_tick) {
        return true;
    }
    if (s_auth_rate.locked_until_tick > 0 && now >= s_auth_rate.locked_until_tick) {
        s_auth_rate.locked_until_tick = 0;
        s_auth_rate.fail_count = 0;
    }
    return false;
}

static void record_auth_fail(void)
{
    s_auth_rate.fail_count++;
    if (s_auth_rate.fail_count >= AUTH_FAIL_LIMIT) {
        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS / 1000;
        s_auth_rate.locked_until_tick = now + AUTH_LOCKOUT_S;
        ESP_LOGW(TAG, "Rate limited: %d auth failures, locked for %d s",
                 s_auth_rate.fail_count, AUTH_LOCKOUT_S);
    }
}

static void record_auth_success(void)
{
    s_auth_rate.fail_count = 0;
    s_auth_rate.locked_until_tick = 0;
}

bool ota_generate_token(char *out, size_t out_sz)
{
    if (!out || out_sz < 17) return false;
    uint32_t r1 = esp_random();
    uint32_t r2 = esp_random();
    snprintf(out, out_sz, "%08lx%08lx", (unsigned long)r1, (unsigned long)r2);
    s_ota_token.created_tick = xTaskGetTickCount();
    strncpy(s_ota_token.token, out, sizeof(s_ota_token.token) - 1);
    s_ota_token.token[sizeof(s_ota_token.token) - 1] = '\0';
    return true;
}

bool ota_check_token(const char *token)
{
    if (!token || s_ota_token.token[0] == '\0') return false;
    TickType_t elapsed = (xTaskGetTickCount() - s_ota_token.created_tick) * portTICK_PERIOD_MS;
    if (elapsed > TOKEN_EXPIRY_S * 1000) {
        ESP_LOGW(TAG, "OTA token expired");
        s_ota_token.token[0] = '\0';
        return false;
    }
    if (ct_memcmp(token, s_ota_token.token, strlen(s_ota_token.token))) {
        s_ota_token.token[0] = '\0';
        return true;
    }
    return false;
}

static bool extract_token_from_uri(httpd_req_t *req, char *token, size_t token_sz)
{
    size_t buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len <= 1) return false;
    char *buf = malloc(buf_len);
    if (!buf) return false;
    if (httpd_req_get_url_query_str(req, buf, buf_len) != ESP_OK) {
        free(buf);
        return false;
    }
    bool found = false;
    if (httpd_query_key_value(buf, "token", token, token_sz) == ESP_OK) {
        found = true;
    }
    free(buf);
    return found;
}

static bool check_basic_auth(httpd_req_t *req)
{
    load_cached_creds();

    if (s_cached_user[0] == '\0' || s_cached_pass[0] == '\0') {
        ESP_LOGE(TAG, "OTA credentials not configured");
        return false;
    }

    char auth_header[256];
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth_header, sizeof(auth_header)) != ESP_OK) {
        return false;
    }
    if (strncmp(auth_header, "Basic ", 6) != 0) return false;

    const char *creds = auth_header + 6;
    size_t creds_len = strlen(creds);

    char expected_creds[256];
    int written = snprintf(expected_creds, sizeof(expected_creds), "%s:%s", s_cached_user, s_cached_pass);
    if (written < 0 || written >= (int)sizeof(expected_creds)) return false;

    char expected_b64[384];
    int n = b64_encode(expected_b64, sizeof(expected_b64),
                       (const uint8_t *)expected_creds, strlen(expected_creds));
    if (n < 0) return false;

    if (creds_len != (size_t)n) {
        size_t cmp_len = creds_len < (size_t)n ? creds_len : (size_t)n;
        if (cmp_len > 0) {
            ct_memcmp(creds, expected_b64, cmp_len);
        }
        return false;
    }
    return ct_memcmp(creds, expected_b64, (size_t)n);
}

static bool check_auth(httpd_req_t *req)
{
    if (is_rate_limited()) {
        ESP_LOGW(TAG, "Auth rate limited");
        return false;
    }

    char token[32] = {0};
    if (extract_token_from_uri(req, token, sizeof(token))) {
        if (ota_check_token(token)) {
            record_auth_success();
            return true;
        }
        ESP_LOGW(TAG, "Invalid or expired OTA token");
    }

    if (check_basic_auth(req)) {
        record_auth_success();
        return true;
    }

    record_auth_fail();
    return false;
}

static void send_401(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"ESP-IDMS OTA\"");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Authentication required");
}

static bool is_end_boundary(const char *line, const char *boundary)
{
    if (strncmp(line, boundary, strlen(boundary)) != 0) return false;
    const char *after = line + strlen(boundary);
    return (strncmp(after, "--", 2) == 0);
}

static const char *find_bytes(const char *buf, size_t buf_len,
                              const char *needle, size_t needle_len)
{
    if (!buf || !needle || needle_len == 0 || buf_len < needle_len) {
        return NULL;
    }
    for (size_t i = 0; i <= buf_len - needle_len; i++) {
        if (memcmp(buf + i, needle, needle_len) == 0) {
            return buf + i;
        }
    }
    return NULL;
}

static esp_err_t ota_write_verified(esp_ota_handle_t ota_h,
                                    mbedtls_sha256_context *sha256_ctx,
                                    const char *data, size_t len,
                                    size_t *total, uint32_t max_size)
{
    if (len == 0) {
        return ESP_OK;
    }
    if (*total + len > max_size) {
        ESP_LOGE(TAG, "Upload exceeds partition size (%zu > %lu)",
                 *total + len, (unsigned long)max_size);
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t err = esp_ota_write(ota_h, data, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
        return err;
    }
    mbedtls_sha256_update(sha256_ctx, (const unsigned char *)data, len);
    *total += len;
    return ESP_OK;
}

static esp_err_t ota_process_file_bytes(const char *data, size_t data_len,
                                        char *scan_buf,
                                        char *tail, size_t *tail_len,
                                        size_t tail_cap,
                                        const char *boundary,
                                        size_t boundary_len,
                                        esp_ota_handle_t ota_h,
                                        mbedtls_sha256_context *sha256_ctx,
                                        size_t *total, uint32_t max_size,
                                        bool *found_boundary)
{
    *found_boundary = false;

    size_t scan_len = *tail_len + data_len;
    memcpy(scan_buf, tail, *tail_len);
    memcpy(scan_buf + *tail_len, data, data_len);

    char delimiter[160];
    int delim_len_i = snprintf(delimiter, sizeof(delimiter), "\r\n%s", boundary);
    if (delim_len_i <= 0 || delim_len_i >= (int)sizeof(delimiter)) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t delimiter_len = (size_t)delim_len_i;

    const char *found = find_bytes(scan_buf, scan_len, delimiter, delimiter_len);
    size_t write_len = 0;
    if (found) {
        write_len = (size_t)(found - scan_buf);
        *found_boundary = true;
    } else if (scan_len >= boundary_len && memcmp(scan_buf, boundary, boundary_len) == 0) {
        write_len = 0;
        *found_boundary = true;
    } else {
        size_t keep_len = (boundary_len + 4 < tail_cap) ? boundary_len + 4 : tail_cap;
        if (scan_len <= keep_len) {
            memcpy(tail, scan_buf, scan_len);
            *tail_len = scan_len;
            return ESP_OK;
        }
        write_len = scan_len - keep_len;
    }

    esp_err_t err = ota_write_verified(ota_h, sha256_ctx, scan_buf, write_len,
                                       total, max_size);
    if (err != ESP_OK) {
        return err;
    }

    if (*found_boundary) {
        *tail_len = 0;
        return ESP_OK;
    }

    *tail_len = scan_len - write_len;
    memcpy(tail, scan_buf + write_len, *tail_len);
    return ESP_OK;
}

static esp_err_t ota_upload_handler(httpd_req_t *req)
{
    if (!check_auth(req)) {
        send_401(req);
        return ESP_OK;
    }

    if (s_update_in_progress) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "OTA update already in progress");
        return ESP_OK;
    }

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
    boundary_start += 9;

    char boundary[128];
    snprintf(boundary, sizeof(boundary), "--%s", boundary_start);
    size_t blen = strlen(boundary);
    while (blen > 0 && (boundary[blen - 1] == '\r' || boundary[blen - 1] == '\n' || boundary[blen - 1] == '\"')) {
        boundary[--blen] = '\0';
    }

    char expected_sha256[65] = {0};
    bool has_expected_hash = false;
    if (httpd_req_get_hdr_value_str(req, "X-Expected-SHA256", expected_sha256, sizeof(expected_sha256)) == ESP_OK) {
        has_expected_hash = true;
        ESP_LOGI(TAG, "Client provided expected SHA256: %s", expected_sha256);
    }

    ESP_LOGI(TAG, "OTA upload started, boundary: %s", boundary);

    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
    if (!update) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "No OTA partition available");
        return ESP_OK;
    }

    uint32_t max_size = update->size;
    ESP_LOGI(TAG, "Writing to partition %s at offset 0x%lx (max %lu bytes)",
             update->label, (unsigned long)update->address, (unsigned long)max_size);

    esp_err_t err = esp_ota_begin(update, OTA_WITH_SEQUENTIAL_WRITES, &s_ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "Failed to begin OTA");
        return ESP_OK;
    }

    s_update_in_progress = true;

    mbedtls_sha256_context sha256_ctx;
    mbedtls_sha256_init(&sha256_ctx);
    mbedtls_sha256_starts(&sha256_ctx, 0);

    char *buf = malloc(OTA_RECV_BUFSZ);
    if (!buf) {
        mbedtls_sha256_free(&sha256_ctx);
        esp_ota_abort(s_ota_handle);
        s_ota_handle = 0;
        s_update_in_progress = false;
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    size_t tail_cap = strlen(boundary) + 4;
    char *tail = malloc(tail_cap);
    char *scan_buf = malloc(OTA_RECV_BUFSZ + tail_cap);
    if (!tail || !scan_buf) {
        free(scan_buf);
        free(tail);
        free(buf);
        mbedtls_sha256_free(&sha256_ctx);
        esp_ota_abort(s_ota_handle);
        s_ota_handle = 0;
        s_update_in_progress = false;
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    size_t total = 0;
    size_t tail_len = 0;
    int recv_len;
    bool in_file_data = false;
    bool skip_headers = true;
    char line_buf[256];
    size_t line_pos = 0;
    bool upload_ok = true;
    bool saw_final_boundary = false;
    esp_ota_handle_t ota_h = s_ota_handle;

    while ((recv_len = httpd_req_recv(req, buf, OTA_RECV_BUFSZ)) > 0) {
        int buf_offset = 0;

        while (buf_offset < recv_len) {
            if (!in_file_data) {
                while (buf_offset < recv_len && line_pos < sizeof(line_buf) - 1) {
                    char c = buf[buf_offset++];
                    line_buf[line_pos++] = c;
                    if (c == '\n') {
                        line_buf[line_pos] = '\0';
                        if (strncmp(line_buf, boundary, strlen(boundary)) == 0) {
                            if (is_end_boundary(line_buf, boundary)) {
                                goto upload_complete;
                            }
                            in_file_data = false;
                            skip_headers = true;
                            line_pos = 0;
                        } else if (skip_headers) {
                            if (line_pos <= 2) {
                                skip_headers = false;
                                in_file_data = true;
                                ESP_LOGI(TAG, "Start of file data");
                            }
                            line_pos = 0;
                        }
                    }
                }
                if (!in_file_data && line_pos >= sizeof(line_buf) - 1) {
                    ESP_LOGE(TAG, "Multipart header line too long");
                    upload_ok = false;
                    goto upload_complete;
                }
            } else {
                size_t remaining = recv_len - buf_offset;
                bool found_boundary = false;
                err = ota_process_file_bytes(buf + buf_offset, remaining,
                                             scan_buf, tail, &tail_len, tail_cap,
                                             boundary, strlen(boundary),
                                             ota_h, &sha256_ctx,
                                             &total, max_size, &found_boundary);
                if (err != ESP_OK) {
                    upload_ok = false;
                    goto upload_complete;
                }
                buf_offset = recv_len;
                if (found_boundary) {
                    saw_final_boundary = true;
                    goto upload_complete;
                }
            }
        }
    }

upload_complete:
    if (recv_len < 0 && upload_ok) {
        upload_ok = false;
    }
    if (!saw_final_boundary && upload_ok) {
        ESP_LOGE(TAG, "Multipart upload ended before final firmware boundary");
        upload_ok = false;
    }

    unsigned char sha256_hash[32];
    mbedtls_sha256_finish(&sha256_ctx, sha256_hash);
    mbedtls_sha256_free(&sha256_ctx);

    char sha256_hex[65];
    for (int i = 0; i < 32; i++) {
        snprintf(sha256_hex + i * 2, 3, "%02x", sha256_hash[i]);
    }
    sha256_hex[64] = '\0';

    if (!upload_ok) {
        esp_ota_abort(ota_h);
        s_ota_handle = 0;
        s_update_in_progress = false;
        free(scan_buf);
        free(tail);
        free(buf);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain");
        char msg[128];
        snprintf(msg, sizeof(msg), "Upload failed (received %zu bytes)", total);
        httpd_resp_sendstr(req, msg);
        return ESP_OK;
    }

    if (has_expected_hash) {
        if (!ct_memcmp(expected_sha256, sha256_hex, 64)) {
            ESP_LOGE(TAG, "SHA256 mismatch: expected %s, got %s", expected_sha256, sha256_hex);
            esp_ota_abort(ota_h);
            s_ota_handle = 0;
            s_update_in_progress = false;
            free(scan_buf);
            free(tail);
            free(buf);
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_set_type(req, "text/plain");
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "SHA256 verification failed.\nExpected: %s\nActual:   %s",
                     expected_sha256, sha256_hex);
            httpd_resp_sendstr(req, msg);
            return ESP_OK;
        }
        ESP_LOGI(TAG, "SHA256 verified: %s", sha256_hex);
    } else {
        ESP_LOGI(TAG, "Upload SHA256: %s (no client verification requested)", sha256_hex);
    }

    err = esp_ota_end(ota_h);
    s_ota_handle = 0;
    s_update_in_progress = false;
    free(scan_buf);
    free(tail);
    free(buf);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "OTA completion failed (bad image?)");
        return ESP_OK;
    }

    const esp_partition_t *update_part = update;
    err = esp_ota_set_boot_partition(update_part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "Failed to set boot partition");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "OTA complete: %zu bytes written, rebooting...", total);

    httpd_resp_set_type(req, "application/json");
    char msg[256];
    snprintf(msg, sizeof(msg),
             "{\"status\":\"success\",\"bytes\":%zu,\"sha256\":\"%s\"}",
             total, sha256_hex);
    httpd_resp_sendstr(req, msg);

    telegram_broadcast_text("\xf0\x9f\x9b\x9c OTA update successful. Rebooting into new firmware.");

    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();

    return ESP_OK;
}

static esp_err_t ota_info_handler(httpd_req_t *req)
{
    if (!check_auth(req)) {
        send_401(req);
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");

    char body[768];
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_app_desc_t app_desc;
    esp_ota_get_partition_description(running, &app_desc);

    char sha256_hex[65] = {0};
    unsigned char hash[32];
    if (esp_partition_get_sha256(running, hash) == ESP_OK) {
        for (int i = 0; i < 32; i++) {
            snprintf(sha256_hex + i * 2, 3, "%02x", hash[i]);
        }
    }

    snprintf(body, sizeof(body),
             "{"
             "\"version\":\"%s\","
             "\"partition\":\"%s\","
             "\"status\":\"%s\","
             "\"idf_version\":\"%s\","
             "\"chip\":\"%s\","
             "\"compile_time\":\"%s %s\","
             "\"sha256\":\"%s\""
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
             app_desc.date, app_desc.time,
             sha256_hex);

    httpd_resp_sendstr(req, body);
    return ESP_OK;
}

static esp_err_t ota_page_handler(httpd_req_t *req)
{
    if (!check_auth(req)) {
        send_401(req);
        return ESP_OK;
    }

#if CONFIG_IDMS_OTA_HTTPS_ENABLE
    const char *proto_note = "<p>Connection is TLS-encrypted (self-signed certificate — accept browser warning).</p>";
#else
    const char *proto_note = "<p>Connection is <b>unencrypted</b> HTTP. Credentials are sent in plaintext.</p>";
#endif

    const char *page =
        "<!DOCTYPE html>"
        "<html><head><meta charset=\"utf-8\"><title>ESP-IDMS OTA Update</title>"
        "<style>"
        "body{font-family:system-ui,sans-serif;max-width:600px;margin:40px auto;padding:0 20px;background:#1a1a2e;color:#eee}"
        "h1{color:#00d4ff}input[type=file]{margin:20px 0}input[type=submit]{"
        "background:#00d4ff;color:#000;border:none;padding:10px 24px;font-size:16px;cursor:pointer;border-radius:4px}"
        "input[type=submit]:hover{background:#00b8d4}"
        ".warn{color:#ff6b6b;font-size:14px;margin-top:16px}"
        ".info{color:#8be9fd;font-size:14px;margin-top:8px}"
        "</style></head><body>"
        "<h1>ESP-IDMS Firmware Update</h1>"
        "<p>Current version: <strong>" PROJECT_VER "</strong></p>"
        "<div class=\"info\">" "%s" "</div>"
        "<form method=\"POST\" enctype=\"multipart/form-data\">"
        "<label>Firmware binary (.bin):</label><br>"
        "<input type=\"file\" name=\"firmware\" accept=\".bin\" required><br>"
        "<input type=\"submit\" value=\"Upload &amp; Update\">"
        "</form>"
        "<p class=\"warn\">&#9888; The device will reboot after a successful update.</p>"
        "</body></html>";

    char *html = malloc(2048);
    if (!html) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    snprintf(html, 2048, page, proto_note);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, html);
    free(html);
    return ESP_OK;
}

static void register_uri_handlers(httpd_handle_t server)
{
    httpd_uri_t page_uri = { .uri = "/", .method = HTTP_GET, .handler = ota_page_handler };
    httpd_register_uri_handler(server, &page_uri);

    httpd_uri_t upload_uri = { .uri = "/", .method = HTTP_POST, .handler = ota_upload_handler };
    httpd_register_uri_handler(server, &upload_uri);

    httpd_uri_t info_uri = { .uri = "/info", .method = HTTP_GET, .handler = ota_info_handler };
    httpd_register_uri_handler(server, &info_uri);
}

#if CONFIG_IDMS_OTA_HTTPS_ENABLE

#include "esp_https_server.h"

extern const unsigned char ota_server_cert_pem_start[] asm("_binary_ota_server_cert_pem_start");
extern const unsigned char ota_server_cert_pem_end[] asm("_binary_ota_server_cert_pem_end");
extern const unsigned char ota_server_key_pem_start[] asm("_binary_ota_server_key_pem_start");
extern const unsigned char ota_server_key_pem_end[] asm("_binary_ota_server_key_pem_end");

static esp_err_t start_ota_http_server(uint16_t port)
{
    httpd_handle_t server = NULL;
    httpd_ssl_config_t ssl_cfg = HTTPD_SSL_CONFIG_DEFAULT();
    ssl_cfg.servercert = ota_server_cert_pem_start;
    ssl_cfg.servercert_len = (ota_server_cert_pem_end - ota_server_cert_pem_start);
    ssl_cfg.prvtkey = ota_server_key_pem_start;
    ssl_cfg.prvtkey_len = (ota_server_key_pem_end - ota_server_key_pem_start);
    ssl_cfg.httpd.server_port = port;
    ssl_cfg.httpd.max_uri_handlers = 8;
    ssl_cfg.httpd.recv_wait_timeout = 30;
    ssl_cfg.httpd.send_wait_timeout = 30;
    ssl_cfg.httpd.max_open_sockets = 4;
    ssl_cfg.httpd.lru_purge_enable = true;

    esp_err_t err = httpd_ssl_start(&server, &ssl_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ssl_start failed: %s", esp_err_to_name(err));
        return err;
    }

    register_uri_handlers(server);
    ESP_LOGI(TAG, "OTA HTTPS server started on port %u", (unsigned)port);
    return ESP_OK;
}

#else

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

    register_uri_handlers(server);
    ESP_LOGI(TAG, "OTA HTTP server started on port %u", (unsigned)port);
    return ESP_OK;
}

#endif

static void check_boot_state(void)
{
    esp_ota_img_states_t state;
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_err_t err = esp_ota_get_state_partition(running, &state);

    if (err == ESP_OK) {
        if (state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGI(TAG, "First boot after OTA on %s — will validate before marking valid", running->label);
            strncpy(s_status, "validating", sizeof(s_status) - 1);
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

static void valid_mark_timer_cb(TimerHandle_t timer)
{
    (void)timer;
    esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGI(TAG, "App marked valid after delay — rollback cancelled");
    strncpy(s_status, "ready", sizeof(s_status) - 1);
}

esp_err_t ota_init(void)
{
    memset(&s_ota_token, 0, sizeof(s_ota_token));
    memset(&s_auth_rate, 0, sizeof(s_auth_rate));

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

const char *ota_get_version(void) { return s_version; }

const char *ota_get_status(void)
{
    if (s_update_in_progress) return "updating";
    return s_status;
}

const char *ota_get_partition(void) { return s_partition; }

void ota_mark_app_valid(void)
{
    esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGI(TAG, "App marked valid — rollback cancelled");
    strncpy(s_status, "ready", sizeof(s_status) - 1);
}

void ota_schedule_valid_mark(uint32_t delay_ms)
{
    esp_ota_img_states_t state;
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        TimerHandle_t timer = xTimerCreate("ota_valid", pdMS_TO_TICKS(delay_ms),
                                           pdFALSE, NULL, valid_mark_timer_cb);
        if (timer) {
            xTimerStart(timer, 0);
            ESP_LOGI(TAG, "OTA valid mark scheduled in %lu ms", (unsigned long)delay_ms);
        } else {
            ESP_LOGE(TAG, "Failed to create OTA valid mark timer");
            ota_mark_app_valid();
        }
    } else {
        ota_mark_app_valid();
    }
}

uint32_t ota_get_max_app_size(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    return running ? running->size : 0;
}
