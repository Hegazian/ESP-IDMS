#include "telegram.h"
#include "config_store.h"
#include "wifi_manager.h"
#include "ota.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "lwip/dns.h"
#include "lwip/ip4_addr.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "telegram";

static size_t append_urlencode_char(char *dst, size_t dst_left, char c)
{
    if (dst_left < 4) {
        return 0;
    }
    const char *hex = "0123456789ABCDEF";
    unsigned char u = (unsigned char)c;
    if ((u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') || u == '-' || u == '_' || u == '.' || u == '~') {
        dst[0] = (char)u;
        return 1;
    }
    if (u == ' ') {
        dst[0] = '+';
        return 1;
    }
    dst[0] = '%';
    dst[1] = hex[(u >> 4) & 0x0F];
    dst[2] = hex[u & 0x0F];
    return 3;
}

static void urlencode_append(char *dst, size_t dst_sz, const char *src)
{
    size_t used = strnlen(dst, dst_sz);
    if (used >= dst_sz) {
        return;
    }
    for (size_t i = 0; src[i] && used + 4 < dst_sz; i++) {
        size_t n = append_urlencode_char(dst + used, dst_sz - used, src[i]);
        if (n == 0) {
            break;
        }
        used += n;
        dst[used] = '\0';
    }
}

static esp_err_t http_post_form(const char *path, const char *body)
{
    if (CONFIG_IDMS_TELEGRAM_BOT_TOKEN[0] == '\0') {
        ESP_LOGW(TAG, "Bot token not configured; skip HTTP");
        return ESP_ERR_INVALID_STATE;
    }

    char url[192];
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s%s", CONFIG_IDMS_TELEGRAM_BOT_TOKEN, path);

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");
    esp_http_client_set_post_field(client, body, (int)strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HTTP perform failed: %s", esp_err_to_name(err));
        return err;
    }
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "HTTP status %d", status);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t telegram_send_text(const char *chat_id, const char *text)
{
    if (!chat_id || !text) {
        return ESP_ERR_INVALID_ARG;
    }
    char body[768];
    snprintf(body, sizeof(body), "chat_id=%s&text=", chat_id);
    urlencode_append(body, sizeof(body), text);
    return http_post_form("/sendMessage", body);
}

esp_err_t telegram_broadcast_text(const char *text)
{
    uint8_t n = config_get_tech_count();
    if (n == 0) {
        ESP_LOGW(TAG, "No technicians configured");
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t last = ESP_OK;
    for (int i = 0; i < n; i++) {
        char id[64];
        if (config_get_tech_id(i, id, sizeof(id)) != ESP_OK) {
            continue;
        }
        esp_err_t e = telegram_send_text(id, text);
        if (e != ESP_OK) {
            last = e;
        }
    }
    return last;
}

esp_err_t telegram_heartbeat(void)
{
    if (CONFIG_IDMS_TELEGRAM_BOT_TOKEN[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    char url[192];
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/getMe", CONFIG_IDMS_TELEGRAM_BOT_TOKEN);

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return ESP_FAIL;
    }
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK) {
        return err;
    }
    if (status < 200 || status >= 300) {
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Heartbeat OK (getMe %d)", status);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Telegram Bot Commands (getUpdates long-polling)                    */
/* ------------------------------------------------------------------ */

static int s_update_offset = -1; /* -1 = not initialized */
static bool s_dns_resolved = false;

/** Check if we can resolve Telegram's DNS */
static bool check_telegram_dns(void)
{
    if (s_dns_resolved) {
        return true;
    }
    ip_addr_t resolved;
    err_t err = dns_gethostbyname("api.telegram.org", &resolved, NULL, NULL);
    if (err == ERR_OK) {
        s_dns_resolved = true;
        char ip_str[16];
        ip4addr_ntoa_r(ip_2_ip4(&resolved), ip_str, sizeof(ip_str));
        ESP_LOGI(TAG, "DNS OK: api.telegram.org → %s", ip_str);
        return true;
    } else if (err == ERR_INPROGRESS) {
        /* Async DNS started — wait briefly */
        vTaskDelay(pdMS_TO_TICKS(2000));
        err = dns_gethostbyname("api.telegram.org", &resolved, NULL, NULL);
        if (err == ERR_OK) {
            s_dns_resolved = true;
            char ip_str[16];
            ip4addr_ntoa_r(ip_2_ip4(&resolved), ip_str, sizeof(ip_str));
            ESP_LOGI(TAG, "DNS OK (delayed): api.telegram.org → %s", ip_str);
            return true;
        }
    }
    ESP_LOGE(TAG, "DNS FAIL: cannot resolve api.telegram.org (err=%d)", err);
    return false;
}

static esp_err_t http_get(const char *path, char *resp_buf, size_t resp_buf_sz, int *status_out)
{
    if (CONFIG_IDMS_TELEGRAM_BOT_TOKEN[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    char url[256];
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s%s", CONFIG_IDMS_TELEGRAM_BOT_TOKEN, path);

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000, /* 15 s timeout — fail fast */
        .event_handler = NULL,
        .user_data = NULL,
        .buffer_size = 512,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "http_client init failed");
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    if (status_out) {
        *status_out = status;
    }

    if (err == ESP_OK && (status >= 200 && status < 300)) {
        int len = esp_http_client_read(client, resp_buf, resp_buf_sz - 1);
        if (len > 0) {
            resp_buf[len] = '\0';
        } else {
            resp_buf[0] = '\0';
        }
    }

    esp_http_client_cleanup(client);
    return err;
}

/**
 * Simple JSON substring search — finds "text":"\/COMMAND" pattern.
 * Returns pointer to the command string after the slash, or NULL.
 */
static const char *find_bot_command(const char *json, const char *cmd)
{
    /* Look for "/cmd" in the JSON response */
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"text\":\"/%s\"", cmd);
    const char *found = strstr(json, pattern);
    if (!found) {
        /* Try with extra chars after command */
        snprintf(pattern, sizeof(pattern), "\"text\":\"/%s ", cmd);
        found = strstr(json, pattern);
    }
    return found;
}

static void command_poll_task(void *arg)
{
    (void)arg;

#if !CONFIG_IDMS_OTA_TELEGRAM_COMMAND
    /* Nothing to do */
    vTaskDelete(NULL);
    return;
#endif

    if (CONFIG_IDMS_TELEGRAM_BOT_TOKEN[0] == '\0') {
        ESP_LOGW(TAG, "Bot token not configured — skipping command poll");
        vTaskDelete(NULL);
        return;
    }

    /* Allocate response buffer from heap to avoid stack pressure */
    char *resp = malloc(2048);
    if (!resp) {
        ESP_LOGE(TAG, "Failed to alloc response buffer");
        vTaskDelete(NULL);
        return;
    }

    const int poll_interval_s = 10;

    ESP_LOGI(TAG, "Telegram command poll started");

    /* Check DNS first to diagnose connectivity */
    if (!check_telegram_dns()) {
        ESP_LOGW(TAG, "Telegram DNS unavailable — skipping poll loop");
        vTaskDelete(NULL);
        return;
    }

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(poll_interval_s * 1000));

        char path[128];
        if (s_update_offset < 0) {
            /* First request — get only the latest update */
            snprintf(path, sizeof(path), "/getUpdates?allowed_updates=[\"message\"]&limit=1");
        } else {
            snprintf(path, sizeof(path), "/getUpdates?allowed_updates=[\"message\"]&offset=%d&limit=5", s_update_offset);
        }

        int status = 0;
        resp[0] = '\0';
        esp_err_t err = http_get(path, resp, 2048, &status);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "getUpdates failed: %s (HTTP %d)", esp_err_to_name(err), status);
            continue;
        }

        if (resp[0] == '\0') {
            continue;
        }

        /* Simple parsing: look for "update_id":NNN to advance offset */
        const char *uid_ptr = strstr(resp, "\"update_id\":");
        if (uid_ptr) {
            s_update_offset = atoi(uid_ptr + 12) + 1;
        }

#if CONFIG_IDMS_OTA_ENABLE
        /* Check for /ota_update command */
        if (find_bot_command(resp, "ota_update")) {
            ESP_LOGI(TAG, "Received /ota_update command");
            ota_trigger_from_telegram();

            char msg[256];
            char ip[16];
            wifi_manager_get_ip(ip, sizeof(ip));
            snprintf(msg, sizeof(msg),
                     "📡 OTA update mode enabled.\n"
                     "Upload firmware via: http://%s:%d/\n"
                     "User: %s  Pass: %s",
                     ip[0] ? ip : "<device-ip>",
                     CONFIG_IDMS_OTA_HTTP_PORT,
                     CONFIG_IDMS_OTA_HTTP_AUTH_USER,
                     CONFIG_IDMS_OTA_HTTP_AUTH_PASS);
            telegram_broadcast_text(msg);
        }
#endif
    }
}

void telegram_command_poll_start(void)
{
    xTaskCreatePinnedToCore(command_poll_task, "tg_cmd", 6144, NULL, 3, NULL, tskNO_AFFINITY);
}
