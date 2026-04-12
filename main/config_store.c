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
