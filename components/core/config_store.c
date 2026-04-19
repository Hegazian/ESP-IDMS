#include "config_store.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_check.h"
#include "sdkconfig.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "cfg";
static const char *NS = "idms";

esp_err_t config_store_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    nvs_handle_t h;
    err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t count = 0;
    err = nvs_get_u8(h, "tech_count", &count);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        count = 0;
    } else if (err != ESP_OK) {
        nvs_close(h);
        return err;
    }

    if (count == 0 && CONFIG_IDMS_DEFAULT_TECH_CHAT_ID[0] != '\0') {
        count = 1;
        ESP_ERROR_CHECK(nvs_set_u8(h, "tech_count", count));
        ESP_ERROR_CHECK(nvs_set_str(h, "tech_id_0", CONFIG_IDMS_DEFAULT_TECH_CHAT_ID));
        ESP_ERROR_CHECK(nvs_commit(h));
        ESP_LOGI(TAG, "Seeded default technician chat id from Kconfig");
    }

    nvs_close(h);
    return ESP_OK;
}

uint8_t config_get_tech_count(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return 0;
    }
    uint8_t count = 0;
    esp_err_t err = nvs_get_u8(h, "tech_count", &count);
    nvs_close(h);
    if (err != ESP_OK) {
        return 0;
    }
    if (count > 5) {
        return 0;
    }
    return count;
}

esp_err_t config_get_tech_id(int idx, char *out, size_t out_len)
{
    if (!out || out_len == 0 || idx < 0 || idx >= 5) {
        return ESP_ERR_INVALID_ARG;
    }
    char key[16];
    snprintf(key, sizeof(key), "tech_id_%d", idx);

    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NS, NVS_READONLY, &h), TAG, "open");

    size_t required = out_len;
    esp_err_t err = nvs_get_str(h, key, out, &required);
    nvs_close(h);
    return err;
}

esp_err_t config_set_tech_id(int idx, const char *id)
{
    if (!id || idx < 0 || idx >= 5) {
        return ESP_ERR_INVALID_ARG;
    }
    char key[16];
    snprintf(key, sizeof(key), "tech_id_%d", idx);

    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NS, NVS_READWRITE, &h), TAG, "open");

    esp_err_t err = nvs_set_str(h, key, id);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t config_add_tech_id(const char *id)
{
    if (!id || id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t count = config_get_tech_count();
    if (count >= 5) {
        return ESP_ERR_NO_MEM;
    }

    for (int i = 0; i < count; i++) {
        char existing[64];
        if (config_get_tech_id(i, existing, sizeof(existing)) == ESP_OK && strcmp(existing, id) == 0) {
            return ESP_OK;
        }
    }

    ESP_RETURN_ON_ERROR(config_set_tech_id((int)count, id), TAG, "set");
    count++;
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NS, NVS_READWRITE, &h), TAG, "open");
    esp_err_t err = nvs_set_u8(h, "tech_count", count);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/* ------------------------------------------------------------------ */
/*  Secrets: Wi-Fi & Telegram credentials stored in NVS "secrets" NS   */
/*  Falls back to Kconfig defaults if not set in NVS.                  */
/* ------------------------------------------------------------------ */

static const char *SECRETS_NS = "secrets";

static esp_err_t secrets_get(const char *key, const char *fallback, char *out, size_t out_len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(SECRETS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        strncpy(out, fallback, out_len - 1);
        out[out_len - 1] = '\0';
        return ESP_OK;
    }
    size_t required = out_len;
    err = nvs_get_str(h, key, out, &required);
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        strncpy(out, fallback, out_len - 1);
        out[out_len - 1] = '\0';
        return ESP_OK;
    }
    return err;
}

static esp_err_t secrets_set(const char *key, const char *value)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(SECRETS_NS, NVS_READWRITE, &h), TAG, "secrets open");
    esp_err_t err = nvs_set_str(h, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t config_get_wifi_ssid(char *out, size_t out_len)
{
    return secrets_get("wifi_ssid", CONFIG_IDMS_WIFI_SSID, out, out_len);
}

esp_err_t config_set_wifi_ssid(const char *ssid)
{
    return secrets_set("wifi_ssid", ssid);
}

esp_err_t config_get_wifi_password(char *out, size_t out_len)
{
    return secrets_get("wifi_pass", CONFIG_IDMS_WIFI_PASSWORD, out, out_len);
}

esp_err_t config_set_wifi_password(const char *password)
{
    return secrets_set("wifi_pass", password);
}

esp_err_t config_get_telegram_token(char *out, size_t out_len)
{
    return secrets_get("tg_token", CONFIG_IDMS_TELEGRAM_BOT_TOKEN, out, out_len);
}

esp_err_t config_set_telegram_token(const char *token)
{
    return secrets_set("tg_token", token);
}

esp_err_t config_get_ota_user(char *out, size_t out_len)
{
    return secrets_get("ota_user", CONFIG_IDMS_OTA_HTTP_AUTH_USER, out, out_len);
}

esp_err_t config_set_ota_user(const char *user)
{
    return secrets_set("ota_user", user);
}

esp_err_t config_get_ota_pass(char *out, size_t out_len)
{
    return secrets_get("ota_pass", CONFIG_IDMS_OTA_HTTP_AUTH_PASS, out, out_len);
}

esp_err_t config_set_ota_pass(const char *pass)
{
    return secrets_set("ota_pass", pass);
}
