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

static void copy_str_or_empty(char *out, size_t out_len, const char *value)
{
    if (!out || out_len == 0) {
        return;
    }
    if (!value) {
        value = "";
    }
    strncpy(out, value, out_len - 1);
    out[out_len - 1] = '\0';
}

static esp_err_t seed_default_str(nvs_handle_t h, const char *key, const char *value, const char *label)
{
    size_t required = 0;
    esp_err_t err = nvs_get_str(h, key, NULL, &required);
    if (err == ESP_OK) {
        return ESP_OK;
    }
    if (err != ESP_ERR_NVS_NOT_FOUND) {
        return err;
    }

    err = nvs_set_str(h, key, value ? value : "");
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Initialized %s to default: %s", label, value ? value : "");
    }
    return err;
}

esp_err_t config_store_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGE(TAG,
                 "NVS init failed with %s. Refusing automatic erase because NVS "
                 "contains provisioned credentials, technicians, calibration, and telemetry state.",
                 esp_err_to_name(err));
        return err;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(err));
        return err;
    }

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

    /* Initialize threshold config defaults if not set */
    int16_t val_i16;
    uint16_t val_u16;
    
    /* Min Temp IN */
    if (nvs_get_i16(h, "min_tin", &val_i16) != ESP_OK) {
        nvs_set_i16(h, "min_tin", CONFIG_DEFAULT_MIN_TIN);
        ESP_LOGI(TAG, "Initialized min_tin to default: %d C", CONFIG_DEFAULT_MIN_TIN);
    }
    
    /* Max Temp IN */
    if (nvs_get_i16(h, "max_tin", &val_i16) != ESP_OK) {
        nvs_set_i16(h, "max_tin", CONFIG_DEFAULT_MAX_TIN);
        ESP_LOGI(TAG, "Initialized max_tin to default: %d C", CONFIG_DEFAULT_MAX_TIN);
    }
    
    /* Min Temp OUT */
    if (nvs_get_i16(h, "min_tout", &val_i16) != ESP_OK) {
        nvs_set_i16(h, "min_tout", CONFIG_DEFAULT_MIN_TOUT);
        ESP_LOGI(TAG, "Initialized min_tout to default: %d C", CONFIG_DEFAULT_MIN_TOUT);
    }
    
    /* Max Temp OUT */
    if (nvs_get_i16(h, "max_tout", &val_i16) != ESP_OK) {
        nvs_set_i16(h, "max_tout", CONFIG_DEFAULT_MAX_TOUT);
        ESP_LOGI(TAG, "Initialized max_tout to default: %d C", CONFIG_DEFAULT_MAX_TOUT);
    }
    
    /* Min Current */
    if (nvs_get_u16(h, "min_curr", &val_u16) != ESP_OK) {
        nvs_set_u16(h, "min_curr", CONFIG_DEFAULT_MIN_CURRENT);
        ESP_LOGI(TAG, "Initialized min_curr to default: %d A", CONFIG_DEFAULT_MIN_CURRENT);
    }
    
    /* Max Current */
    if (nvs_get_u16(h, "max_curr", &val_u16) != ESP_OK) {
        nvs_set_u16(h, "max_curr", CONFIG_DEFAULT_MAX_CURRENT);
        ESP_LOGI(TAG, "Initialized max_curr to default: %d A", CONFIG_DEFAULT_MAX_CURRENT);
    }

    /* Power-loss threshold */
    if (nvs_get_u16(h, "power_ma", &val_u16) != ESP_OK) {
        nvs_set_u16(h, "power_ma", CONFIG_IDMS_CURRENT_THRESHOLD_MA);
        ESP_LOGI(TAG, "Initialized power_ma to default: %d mA", CONFIG_IDMS_CURRENT_THRESHOLD_MA);
    }

    /* Machine-running threshold used to gate cooling alerts */
    if (nvs_get_u16(h, "run_ma", &val_u16) != ESP_OK) {
        nvs_set_u16(h, "run_ma", CONFIG_DEFAULT_MIN_CURRENT * 1000);
        ESP_LOGI(TAG, "Initialized run_ma to default: %d mA", CONFIG_DEFAULT_MIN_CURRENT * 1000);
    }
    
    /* Delta alert threshold */
    if (nvs_get_i16(h, "dt_alert", &val_i16) != ESP_OK) {
        nvs_set_i16(h, "dt_alert", CONFIG_DEFAULT_DT_ALERT_THRESHOLD);
        ESP_LOGI(TAG, "Initialized dt_alert to default: %d C", CONFIG_DEFAULT_DT_ALERT_THRESHOLD);
    }

    /* Delta high alert threshold */
    if (nvs_get_i16(h, "dt_high", &val_i16) != ESP_OK) {
        nvs_set_i16(h, "dt_high", CONFIG_IDMS_DT_HIGH_C);
        ESP_LOGI(TAG, "Initialized dt_high to default: %d C", CONFIG_IDMS_DT_HIGH_C);
    }

    err = seed_default_str(h, "dev_model", CONFIG_DEFAULT_DEVICE_MODEL, "device model");
    if (err != ESP_OK) {
        nvs_close(h);
        return err;
    }
    err = seed_default_str(h, "sup_email", CONFIG_DEFAULT_SUPPORT_EMAIL, "support email");
    if (err != ESP_OK) {
        nvs_close(h);
        return err;
    }
    err = seed_default_str(h, "sup_phone", CONFIG_DEFAULT_SUPPORT_PHONE, "support phone");
    if (err != ESP_OK) {
        nvs_close(h);
        return err;
    }
    err = seed_default_str(h, "qr_code", CONFIG_DEFAULT_QR_CODE, "QR code");
    if (err != ESP_OK) {
        nvs_close(h);
        return err;
    }
    char qr_value[CONFIG_INFO_STRING_MAX_LEN + 1] = {0};
    size_t qr_len = sizeof(qr_value);
    if (nvs_get_str(h, "qr_code", qr_value, &qr_len) == ESP_OK &&
        strcmp(qr_value, "https://t.me/IDMS_UserBot") == 0) {
        nvs_set_str(h, "qr_code", CONFIG_DEFAULT_QR_CODE);
        ESP_LOGI(TAG, "Migrated QR code default to %s", CONFIG_DEFAULT_QR_CODE);
    }
    
    nvs_commit(h);
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

    /* Validate ID length to prevent buffer overflow */
    size_t id_len = strlen(id);
    if (id_len < 3 || id_len >= 60) {
        ESP_LOGE(TAG, "Invalid technician ID length: %zu (must be 3-59 chars)", id_len);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t count = config_get_tech_count();
    if (count >= 5) {
        ESP_LOGW(TAG, "Maximum technician IDs (5) reached");
        return ESP_ERR_NO_MEM;
    }

    /* Check for duplicates */
    for (int i = 0; i < count; i++) {
        char existing[64];
        if (config_get_tech_id(i, existing, sizeof(existing)) == ESP_OK && strcmp(existing, id) == 0) {
            ESP_LOGI(TAG, "Technician ID already exists: %s", id);
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
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Added technician ID: %s (count: %u)", id, count);
    }
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

esp_err_t config_get_cloud_url(char *out, size_t out_len)
{
    return secrets_get("cloud_url", CONFIG_IDMS_CLOUD_URL, out, out_len);
}

esp_err_t config_set_cloud_url(const char *url)
{
    return secrets_set("cloud_url", url);
}

esp_err_t config_get_cloud_token(char *out, size_t out_len)
{
    return secrets_get("cloud_token", CONFIG_IDMS_CLOUD_TOKEN, out, out_len);
}

esp_err_t config_set_cloud_token(const char *token)
{
    return secrets_set("cloud_token", token);
}

/* ------------------------------------------------------------------ */
/*  Device information stored in NVS "idms" namespace                  */
/* ------------------------------------------------------------------ */

static esp_err_t store_get_str_or_default(const char *key, const char *fallback,
                                          char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        copy_str_or_empty(out, out_len, fallback);
        return ESP_OK;
    }

    size_t required = out_len;
    err = nvs_get_str(h, key, out, &required);
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        copy_str_or_empty(out, out_len, fallback);
        return ESP_OK;
    }
    return err;
}

static esp_err_t store_set_str(const char *key, const char *value)
{
    if (!value) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(value) > CONFIG_INFO_STRING_MAX_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }

    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NS, NVS_READWRITE, &h), TAG, "open");
    esp_err_t err = nvs_set_str(h, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t config_get_device_model(char *out, size_t out_len)
{
    return store_get_str_or_default("dev_model", CONFIG_DEFAULT_DEVICE_MODEL, out, out_len);
}

esp_err_t config_set_device_model(const char *value)
{
    return store_set_str("dev_model", value);
}

esp_err_t config_get_serial_number(char *out, size_t out_len)
{
    return store_get_str_or_default("serial_no", "", out, out_len);
}

esp_err_t config_set_serial_number(const char *value)
{
    return store_set_str("serial_no", value);
}

esp_err_t config_get_manufacture_date(char *out, size_t out_len)
{
    return store_get_str_or_default("mfg_date", "", out, out_len);
}

esp_err_t config_set_manufacture_date(const char *value)
{
    return store_set_str("mfg_date", value);
}

esp_err_t config_get_support_email(char *out, size_t out_len)
{
    return store_get_str_or_default("sup_email", CONFIG_DEFAULT_SUPPORT_EMAIL, out, out_len);
}

esp_err_t config_set_support_email(const char *value)
{
    return store_set_str("sup_email", value);
}

esp_err_t config_get_support_phone(char *out, size_t out_len)
{
    return store_get_str_or_default("sup_phone", CONFIG_DEFAULT_SUPPORT_PHONE, out, out_len);
}

esp_err_t config_set_support_phone(const char *value)
{
    return store_set_str("sup_phone", value);
}

esp_err_t config_get_qr_code(char *out, size_t out_len)
{
    return store_get_str_or_default("qr_code", CONFIG_DEFAULT_QR_CODE, out, out_len);
}

esp_err_t config_set_qr_code(const char *value)
{
    return store_set_str("qr_code", value);
}

/* ------------------------------------------------------------------ */
/*  Threshold configuration helpers                                    */
/* ------------------------------------------------------------------ */

static int16_t nvs_get_i16_or_default(const char *key, int16_t default_val)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return default_val;
    }
    int16_t value = default_val;
    esp_err_t err = nvs_get_i16(h, key, &value);
    nvs_close(h);
    if (err != ESP_OK) {
        return default_val;
    }
    return value;
}

static uint16_t nvs_get_u16_or_default(const char *key, uint16_t default_val)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return default_val;
    }
    uint16_t value = default_val;
    esp_err_t err = nvs_get_u16(h, key, &value);
    nvs_close(h);
    if (err != ESP_OK) {
        return default_val;
    }
    return value;
}

static uint32_t nvs_get_u32_or_default(const char *key, uint32_t default_val)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return default_val;
    }
    uint32_t value = default_val;
    esp_err_t err = nvs_get_u32(h, key, &value);
    nvs_close(h);
    if (err != ESP_OK) {
        return default_val;
    }
    return value;
}

static esp_err_t store_set_i16(const char *key, int16_t value)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NS, NVS_READWRITE, &h), TAG, "open");
    esp_err_t err = nvs_set_i16(h, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static esp_err_t store_set_u16(const char *key, uint16_t value)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NS, NVS_READWRITE, &h), TAG, "open");
    esp_err_t err = nvs_set_u16(h, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static esp_err_t store_set_u32(const char *key, uint32_t value)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NS, NVS_READWRITE, &h), TAG, "open");
    esp_err_t err = nvs_set_u32(h, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/* ------------------------------------------------------------------ */
/*  Threshold configuration getters/setters                            */
/* ------------------------------------------------------------------ */

int16_t config_get_min_tin(void)
{
    return nvs_get_i16_or_default("min_tin", CONFIG_DEFAULT_MIN_TIN);
}

esp_err_t config_set_min_tin(int16_t value)
{
    if (value < CONFIG_TEMP_MIN_LIMIT || value > CONFIG_TEMP_MAX_LIMIT) {
        return ESP_ERR_INVALID_ARG;
    }
    return store_set_i16("min_tin", value);
}

int16_t config_get_max_tin(void)
{
    return nvs_get_i16_or_default("max_tin", CONFIG_DEFAULT_MAX_TIN);
}

esp_err_t config_set_max_tin(int16_t value)
{
    if (value < CONFIG_TEMP_MIN_LIMIT || value > CONFIG_TEMP_MAX_LIMIT) {
        return ESP_ERR_INVALID_ARG;
    }
    return store_set_i16("max_tin", value);
}

int16_t config_get_min_tout(void)
{
    return nvs_get_i16_or_default("min_tout", CONFIG_DEFAULT_MIN_TOUT);
}

esp_err_t config_set_min_tout(int16_t value)
{
    if (value < CONFIG_TEMP_MIN_LIMIT || value > CONFIG_TEMP_MAX_LIMIT) {
        return ESP_ERR_INVALID_ARG;
    }
    return store_set_i16("min_tout", value);
}

int16_t config_get_max_tout(void)
{
    return nvs_get_i16_or_default("max_tout", CONFIG_DEFAULT_MAX_TOUT);
}

esp_err_t config_set_max_tout(int16_t value)
{
    if (value < CONFIG_TEMP_MIN_LIMIT || value > CONFIG_TEMP_MAX_LIMIT) {
        return ESP_ERR_INVALID_ARG;
    }
    return store_set_i16("max_tout", value);
}

uint16_t config_get_min_current(void)
{
    return nvs_get_u16_or_default("min_curr", CONFIG_DEFAULT_MIN_CURRENT);
}

esp_err_t config_set_min_current(uint16_t value)
{
    if (value > CONFIG_CURRENT_MAX_LIMIT) {
        return ESP_ERR_INVALID_ARG;
    }
    return store_set_u16("min_curr", value);
}

uint16_t config_get_max_current(void)
{
    return nvs_get_u16_or_default("max_curr", CONFIG_DEFAULT_MAX_CURRENT);
}

esp_err_t config_set_max_current(uint16_t value)
{
    if (value > CONFIG_CURRENT_MAX_LIMIT) {
        return ESP_ERR_INVALID_ARG;
    }
    return store_set_u16("max_curr", value);
}

uint16_t config_get_power_loss_current_ma(void)
{
    return nvs_get_u16_or_default("power_ma", CONFIG_IDMS_CURRENT_THRESHOLD_MA);
}

esp_err_t config_set_power_loss_current_ma(uint16_t value)
{
    if (value > (uint16_t)(CONFIG_CURRENT_MAX_LIMIT * 1000)) {
        return ESP_ERR_INVALID_ARG;
    }
    return store_set_u16("power_ma", value);
}

uint16_t config_get_machine_running_current_ma(void)
{
    return nvs_get_u16_or_default("run_ma", CONFIG_DEFAULT_MIN_CURRENT * 1000);
}

esp_err_t config_set_machine_running_current_ma(uint16_t value)
{
    if (value > (uint16_t)(CONFIG_CURRENT_MAX_LIMIT * 1000)) {
        return ESP_ERR_INVALID_ARG;
    }
    return store_set_u16("run_ma", value);
}

int16_t config_get_dt_alert_threshold(void)
{
    return nvs_get_i16_or_default("dt_alert", CONFIG_DEFAULT_DT_ALERT_THRESHOLD);
}

esp_err_t config_set_dt_alert_threshold(int16_t value)
{
    if (value < 0 || value > 100) {  /* 0 to 100 C (pure value) */
        return ESP_ERR_INVALID_ARG;
    }
    return store_set_i16("dt_alert", value);
}

int16_t config_get_dt_high_threshold(void)
{
    return nvs_get_i16_or_default("dt_high", CONFIG_IDMS_DT_HIGH_C);
}

esp_err_t config_set_dt_high_threshold(int16_t value)
{
    if (value < 0 || value > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    return store_set_i16("dt_high", value);
}

uint32_t config_get_current_cal_x100(void)
{
    return nvs_get_u32_or_default("curr_cal", CONFIG_IDMS_CT_AMPS_PER_VOLT_X100);
}

esp_err_t config_set_current_cal_x100(uint32_t value)
{
    if (value < 10 || value > 500000) {
        return ESP_ERR_INVALID_ARG;
    }
    return store_set_u32("curr_cal", value);
}
