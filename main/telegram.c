#include "telegram.h"
#include "config_store.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "sdkconfig.h"
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
