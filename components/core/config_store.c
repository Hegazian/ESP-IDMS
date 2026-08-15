#include "config_store.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_random.h"
#include "sdkconfig.h"
#include "mbedtls/sha256.h"
#include "esp_mac.h"
#include <ctype.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "cfg";
static const char *NS = "idms";
static const char *SECRETS_NS = "secrets";

#define TECH_ID_MAX_LEN        63
#define TECH_PHONE_MAX_LEN     15
#define TECH_PASSWORD_MIN_LEN  6
#define TECH_SALT_HEX_LEN      32
#define TECH_HASH_HEX_LEN      64

static void hex_encode(const uint8_t *src, size_t src_len, char *dst, size_t dst_len)
{
    static const char hex[] = "0123456789abcdef";
    if (!dst || dst_len < (src_len * 2 + 1)) {
        return;
    }
    for (size_t i = 0; i < src_len; i++) {
        dst[i * 2] = hex[(src[i] >> 4) & 0x0f];
        dst[i * 2 + 1] = hex[src[i] & 0x0f];
    }
    dst[src_len * 2] = '\0';
}

static void random_hex(char *out, size_t out_len, size_t random_bytes)
{
    uint8_t bytes[32];
    if (!out || out_len < random_bytes * 2 + 1 || random_bytes > sizeof(bytes)) {
        return;
    }
    for (size_t i = 0; i < random_bytes; i += 4) {
        uint32_t r = esp_random();
        size_t left = random_bytes - i;
        size_t chunk = left < sizeof(r) ? left : sizeof(r);
        memcpy(bytes + i, &r, chunk);
    }
    hex_encode(bytes, random_bytes, out, out_len);
}

static bool valid_tech_name(const char *name)
{
    if (!name) {
        return true;
    }
    size_t len = strlen(name);
    if (len > CONFIG_TECH_NAME_MAX_LEN) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20 || c == 0x7f) {
            return false;
        }
    }
    return true;
}

static bool valid_password(const char *password)
{
    if (!password) {
        return false;
    }
    size_t len = strlen(password);
    return len >= TECH_PASSWORD_MIN_LEN && len <= CONFIG_TECH_PASSWORD_MAX_LEN;
}

static void tech_key(char *out, size_t out_len, const char *prefix, int idx)
{
    snprintf(out, out_len, "%s_%d", prefix, idx);
}

static esp_err_t password_hash_hex(const char *salt_hex, const char *password,
                                   char *hash_hex, size_t hash_hex_len)
{
    if (!salt_hex || !password || !hash_hex || hash_hex_len < TECH_HASH_HEX_LEN + 1) {
        return ESP_ERR_INVALID_ARG;
    }

    unsigned char hash[32];
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, (const unsigned char *)salt_hex, strlen(salt_hex));
    mbedtls_sha256_update(&ctx, (const unsigned char *)":", 1);
    mbedtls_sha256_update(&ctx, (const unsigned char *)password, strlen(password));
    mbedtls_sha256_finish(&ctx, hash);
    mbedtls_sha256_free(&ctx);

    hex_encode(hash, sizeof(hash), hash_hex, hash_hex_len);
    return ESP_OK;
}

static bool ct_equal_hex(const char *a, const char *b, size_t len)
{
    if (!a || !b) {
        return false;
    }
    unsigned char diff = 0;
    for (size_t i = 0; i < len; i++) {
        diff |= (unsigned char)(a[i] ^ b[i]);
    }
    return diff == 0;
}

static esp_err_t keep_first_err(esp_err_t current, esp_err_t next)
{
    return current == ESP_OK ? next : current;
}

static esp_err_t erase_key_if_exists(nvs_handle_t h, const char *key)
{
    esp_err_t err = nvs_erase_key(h, key);
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
}

static esp_err_t erase_tech_slot(nvs_handle_t h, int idx)
{
    char key[20];
    esp_err_t err = ESP_OK;
    tech_key(key, sizeof(key), "tech_id", idx);
    err = keep_first_err(err, erase_key_if_exists(h, key));
    tech_key(key, sizeof(key), "tech_name", idx);
    err = keep_first_err(err, erase_key_if_exists(h, key));
    tech_key(key, sizeof(key), "tech_salt", idx);
    err = keep_first_err(err, erase_key_if_exists(h, key));
    tech_key(key, sizeof(key), "tech_pwd", idx);
    err = keep_first_err(err, erase_key_if_exists(h, key));
    tech_key(key, sizeof(key), "tech_phone", idx);
    err = keep_first_err(err, erase_key_if_exists(h, key));
    return err;
}

static esp_err_t move_str_key(nvs_handle_t h, const char *prefix, int dst_idx, int src_idx, size_t buf_len)
{
    char src_key[20];
    char dst_key[20];
    char val[96];
    if (buf_len > sizeof(val)) {
        buf_len = sizeof(val);
    }
    tech_key(src_key, sizeof(src_key), prefix, src_idx);
    tech_key(dst_key, sizeof(dst_key), prefix, dst_idx);
    size_t len = buf_len;
    esp_err_t err = nvs_get_str(h, src_key, val, &len);
    if (err == ESP_OK) {
        return nvs_set_str(h, dst_key, val);
    }
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return erase_key_if_exists(h, dst_key);
    }
    return err;
}

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

    /* Temperature calibration offsets */
    if (nvs_get_i16(h, "tin_off_x10", &val_i16) != ESP_OK) {
        nvs_set_i16(h, "tin_off_x10", CONFIG_DEFAULT_TEMP_OFFSET_X10);
        ESP_LOGI(TAG, "Initialized tin_off_x10 to default: %d", CONFIG_DEFAULT_TEMP_OFFSET_X10);
    }
    if (nvs_get_i16(h, "tout_off_x10", &val_i16) != ESP_OK) {
        nvs_set_i16(h, "tout_off_x10", CONFIG_DEFAULT_TEMP_OFFSET_X10);
        ESP_LOGI(TAG, "Initialized tout_off_x10 to default: %d", CONFIG_DEFAULT_TEMP_OFFSET_X10);
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
        (strcmp(qr_value, "@IDMS_USERBOT") == 0 ||
         strcmp(qr_value, "https://t.me/IDMS_UserBot") == 0)) {
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
    if (count > CONFIG_TECH_MAX_COUNT) {
        return 0;
    }
    return count;
}

esp_err_t config_get_tech_id(int idx, char *out, size_t out_len)
{
    if (!out || out_len == 0 || idx < 0 || idx >= CONFIG_TECH_MAX_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    char key[16];
    tech_key(key, sizeof(key), "tech_id", idx);

    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NS, NVS_READONLY, &h), TAG, "open");

    size_t required = out_len;
    esp_err_t err = nvs_get_str(h, key, out, &required);
    nvs_close(h);
    return err;
}

esp_err_t config_set_tech_id(int idx, const char *id)
{
    if (!id || idx < 0 || idx >= CONFIG_TECH_MAX_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    char key[16];
    tech_key(key, sizeof(key), "tech_id", idx);

    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NS, NVS_READWRITE, &h), TAG, "open");

    esp_err_t err = nvs_set_str(h, key, id);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t config_find_tech_id(const char *id, int *idx_out)
{
    if (!id || id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t count = config_get_tech_count();
    for (int i = 0; i < count; i++) {
        char existing[64];
        if (config_get_tech_id(i, existing, sizeof(existing)) == ESP_OK &&
            strcmp(existing, id) == 0) {
            if (idx_out) {
                *idx_out = i;
            }
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t config_get_tech_name(int idx, char *out, size_t out_len)
{
    if (!out || out_len == 0 || idx < 0 || idx >= CONFIG_TECH_MAX_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';

    char key[20];
    tech_key(key, sizeof(key), "tech_name", idx);

    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NS, NVS_READONLY, &h), TAG, "open");

    size_t required = out_len;
    esp_err_t err = nvs_get_str(h, key, out, &required);
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        out[0] = '\0';
        return ESP_OK;
    }
    return err;
}

esp_err_t config_set_tech_name(int idx, const char *name)
{
    if (idx < 0 || idx >= CONFIG_TECH_MAX_COUNT || !valid_tech_name(name)) {
        return ESP_ERR_INVALID_ARG;
    }

    char key[20];
    tech_key(key, sizeof(key), "tech_name", idx);

    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NS, NVS_READWRITE, &h), TAG, "open");

    esp_err_t err;
    if (name && name[0] != '\0') {
        err = nvs_set_str(h, key, name);
    } else {
        err = nvs_erase_key(h, key);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t config_normalize_egypt_phone(const char *input, char *out, size_t out_len)
{
    if (!input || !out || out_len < TECH_PHONE_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    char digits[24] = "";
    size_t used = 0;
    for (const char *p = input; *p && used + 1 < sizeof(digits); p++) {
        if (isdigit((unsigned char)*p)) {
            digits[used++] = *p;
        } else if (*p == '+' || *p == ' ' || *p == '-' || *p == '(' || *p == ')') {
            continue;
        } else {
            return ESP_ERR_INVALID_ARG;
        }
    }
    digits[used] = '\0';

    const char *local = NULL;
    if (used == 11 && digits[0] == '0' && digits[1] == '1') {
        local = digits + 1;
    } else if (used == 10 && digits[0] == '1') {
        local = digits;
    } else if (used == 12 && strncmp(digits, "20", 2) == 0 && digits[2] == '1') {
        local = digits + 2;
    } else if (used == 13 && strncmp(digits, "200", 3) == 0 && digits[3] == '1') {
        local = digits + 3;
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    if (strlen(local) != 10 ||
        !(strncmp(local, "10", 2) == 0 ||
          strncmp(local, "11", 2) == 0 ||
          strncmp(local, "12", 2) == 0 ||
          strncmp(local, "15", 2) == 0)) {
        return ESP_ERR_INVALID_ARG;
    }

    snprintf(out, out_len, "+20%s", local);
    return ESP_OK;
}

esp_err_t config_get_tech_phone(int idx, char *out, size_t out_len)
{
    if (!out || out_len == 0 || idx < 0 || idx >= CONFIG_TECH_MAX_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';

    char key[20];
    tech_key(key, sizeof(key), "tech_phone", idx);

    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NS, NVS_READONLY, &h), TAG, "open");

    size_t required = out_len;
    esp_err_t err = nvs_get_str(h, key, out, &required);
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        out[0] = '\0';
        return ESP_OK;
    }
    return err;
}

esp_err_t config_set_tech_phone(int idx, const char *phone)
{
    if (idx < 0 || idx >= CONFIG_TECH_MAX_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    char normalized[TECH_PHONE_MAX_LEN] = "";
    if (phone && phone[0] != '\0') {
        ESP_RETURN_ON_ERROR(config_normalize_egypt_phone(phone, normalized, sizeof(normalized)),
                            TAG, "normalize phone");
    }

    char key[20];
    tech_key(key, sizeof(key), "tech_phone", idx);

    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NS, NVS_READWRITE, &h), TAG, "open");

    esp_err_t err;
    if (normalized[0] != '\0') {
        err = nvs_set_str(h, key, normalized);
    } else {
        err = nvs_erase_key(h, key);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t config_find_tech_phone(const char *phone, int *idx_out)
{
    char normalized[TECH_PHONE_MAX_LEN] = "";
    ESP_RETURN_ON_ERROR(config_normalize_egypt_phone(phone, normalized, sizeof(normalized)),
                        TAG, "normalize phone");

    uint8_t count = config_get_tech_count();
    for (int i = 0; i < count; i++) {
        char existing[TECH_PHONE_MAX_LEN] = "";
        if (config_get_tech_phone(i, existing, sizeof(existing)) == ESP_OK &&
            strcmp(existing, normalized) == 0) {
            if (idx_out) {
                *idx_out = i;
            }
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t config_add_pending_tech_phone(const char *phone, const char *name)
{
    if (!valid_tech_name(name)) {
        return ESP_ERR_INVALID_ARG;
    }

    char normalized[TECH_PHONE_MAX_LEN] = "";
    ESP_RETURN_ON_ERROR(config_normalize_egypt_phone(phone, normalized, sizeof(normalized)),
                        TAG, "normalize phone");

    int existing_idx = -1;
    if (config_find_tech_phone(normalized, &existing_idx) == ESP_OK) {
        if (name && name[0] != '\0') {
            ESP_RETURN_ON_ERROR(config_set_tech_name(existing_idx, name), TAG, "set name");
        }
        return ESP_OK;
    }

    uint8_t count = config_get_tech_count();
    if (count >= CONFIG_TECH_MAX_COUNT) {
        return ESP_ERR_NO_MEM;
    }

    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NS, NVS_READWRITE, &h), TAG, "open");

    char key[20];
    tech_key(key, sizeof(key), "tech_id", count);
    esp_err_t err = nvs_set_str(h, key, "");
    if (err == ESP_OK) {
        tech_key(key, sizeof(key), "tech_phone", count);
        err = nvs_set_str(h, key, normalized);
    }
    if (err == ESP_OK && name && name[0] != '\0') {
        tech_key(key, sizeof(key), "tech_name", count);
        err = nvs_set_str(h, key, name);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, "tech_count", count + 1);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Added pending technician phone: %s (count: %u)", normalized, count + 1);
    }
    return err;
}

esp_err_t config_bind_tech_phone(const char *phone, const char *telegram_id, const char *name)
{
    if (!telegram_id || telegram_id[0] == '\0' || !valid_tech_name(name)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(telegram_id) < 3 || strlen(telegram_id) > TECH_ID_MAX_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }

    char normalized[TECH_PHONE_MAX_LEN] = "";
    ESP_RETURN_ON_ERROR(config_normalize_egypt_phone(phone, normalized, sizeof(normalized)),
                        TAG, "normalize phone");

    int phone_idx = -1;
    bool phone_exists = (config_find_tech_phone(normalized, &phone_idx) == ESP_OK);

    int id_idx = -1;
    bool id_exists = (config_find_tech_id(telegram_id, &id_idx) == ESP_OK);

    if (phone_exists && id_exists && phone_idx != id_idx) {
        return ESP_ERR_INVALID_STATE;
    }

    int idx = phone_exists ? phone_idx : id_idx;
    if (idx >= 0) {
        char existing_id[64] = "";
        char existing_phone[TECH_PHONE_MAX_LEN] = "";
        config_get_tech_id(idx, existing_id, sizeof(existing_id));
        config_get_tech_phone(idx, existing_phone, sizeof(existing_phone));
        if (existing_id[0] != '\0' && strcmp(existing_id, telegram_id) != 0) {
            return ESP_ERR_INVALID_STATE;
        }
        if (existing_phone[0] != '\0' && strcmp(existing_phone, normalized) != 0) {
            return ESP_ERR_INVALID_STATE;
        }
        ESP_RETURN_ON_ERROR(config_set_tech_id(idx, telegram_id), TAG, "bind id");
        ESP_RETURN_ON_ERROR(config_set_tech_phone(idx, normalized), TAG, "bind phone");
    } else {
        uint8_t count = config_get_tech_count();
        if (count >= CONFIG_TECH_MAX_COUNT) {
            return ESP_ERR_NO_MEM;
        }

        nvs_handle_t h;
        ESP_RETURN_ON_ERROR(nvs_open(NS, NVS_READWRITE, &h), TAG, "open");

        char key[20];
        tech_key(key, sizeof(key), "tech_id", count);
        esp_err_t err = nvs_set_str(h, key, telegram_id);
        if (err == ESP_OK) {
            tech_key(key, sizeof(key), "tech_phone", count);
            err = nvs_set_str(h, key, normalized);
        }
        if (err == ESP_OK && name && name[0] != '\0') {
            tech_key(key, sizeof(key), "tech_name", count);
            err = nvs_set_str(h, key, name);
        }
        if (err == ESP_OK) {
            err = nvs_set_u8(h, "tech_count", count + 1);
        }
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
        if (err != ESP_OK) {
            return err;
        }
        idx = count;
    }

    if (name && name[0] != '\0') {
        ESP_RETURN_ON_ERROR(config_set_tech_name(idx, name), TAG, "set name");
    }
    ESP_LOGI(TAG, "Bound technician phone %s to Telegram ID %s", normalized, telegram_id);
    return ESP_OK;
}

esp_err_t config_add_tech(const char *id, const char *name)
{
    if (!id || id[0] == '\0' || !valid_tech_name(name)) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Validate ID length to prevent buffer overflow */
    size_t id_len = strlen(id);
    if (id_len < 3 || id_len > TECH_ID_MAX_LEN) {
        ESP_LOGE(TAG, "Invalid technician ID length: %zu (must be 3-59 chars)", id_len);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t count = config_get_tech_count();

    /* Check for duplicates */
    for (int i = 0; i < count; i++) {
        char existing[64];
        if (config_get_tech_id(i, existing, sizeof(existing)) == ESP_OK && strcmp(existing, id) == 0) {
            ESP_LOGI(TAG, "Technician ID already exists: %s", id);
            if (name && name[0] != '\0') {
                ESP_RETURN_ON_ERROR(config_set_tech_name(i, name), TAG, "set name");
            }
            return ESP_OK;
        }
    }

    if (count >= CONFIG_TECH_MAX_COUNT) {
        ESP_LOGW(TAG, "Maximum technician IDs (%d) reached", CONFIG_TECH_MAX_COUNT);
        return ESP_ERR_NO_MEM;
    }

    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NS, NVS_READWRITE, &h), TAG, "open");

    char key[20];
    tech_key(key, sizeof(key), "tech_id", count);
    esp_err_t err = nvs_set_str(h, key, id);
    if (err == ESP_OK && name && name[0] != '\0') {
        tech_key(key, sizeof(key), "tech_name", count);
        err = nvs_set_str(h, key, name);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, "tech_count", count + 1);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Added technician ID: %s (count: %u)", id, count + 1);
    }
    return err;
}

esp_err_t config_add_tech_id(const char *id)
{
    return config_add_tech(id, "");
}

esp_err_t config_remove_tech(int idx)
{
    uint8_t count = config_get_tech_count();
    if (idx < 0 || idx >= count) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NS, NVS_READWRITE, &h), TAG, "open");

    esp_err_t err = ESP_OK;
    for (int i = idx; i < count - 1 && err == ESP_OK; i++) {
        err = keep_first_err(err, move_str_key(h, "tech_id", i, i + 1, TECH_ID_MAX_LEN + 1));
        err = keep_first_err(err, move_str_key(h, "tech_name", i, i + 1, CONFIG_TECH_NAME_MAX_LEN + 1));
        err = keep_first_err(err, move_str_key(h, "tech_salt", i, i + 1, TECH_SALT_HEX_LEN + 1));
        err = keep_first_err(err, move_str_key(h, "tech_pwd", i, i + 1, TECH_HASH_HEX_LEN + 1));
        err = keep_first_err(err, move_str_key(h, "tech_phone", i, i + 1, TECH_PHONE_MAX_LEN));
    }
    if (err == ESP_OK) {
        err = erase_tech_slot(h, count - 1);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, "tech_count", count - 1);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t config_clear_techs(void)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NS, NVS_READWRITE, &h), TAG, "open");
    esp_err_t err = ESP_OK;
    for (int i = 0; i < CONFIG_TECH_MAX_COUNT && err == ESP_OK; i++) {
        err = erase_tech_slot(h, i);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, "tech_count", 0);
    }
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

esp_err_t config_get_telegram_admin_name(char *out, size_t out_len)
{
    return secrets_get("tg_admin_name", "", out, out_len);
}

esp_err_t config_set_telegram_admin_name(const char *name)
{
    if (!name || name[0] == '\0' || !valid_tech_name(name)) {
        return ESP_ERR_INVALID_ARG;
    }
    return secrets_set("tg_admin_name", name);
}

bool config_has_telegram_admin_password(void)
{
    nvs_handle_t h;
    if (nvs_open(SECRETS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }

    char salt[TECH_SALT_HEX_LEN + 1] = {0};
    char hash[TECH_HASH_HEX_LEN + 1] = {0};
    size_t len = sizeof(salt);
    esp_err_t salt_err = nvs_get_str(h, "tg_admin_salt", salt, &len);
    len = sizeof(hash);
    esp_err_t hash_err = nvs_get_str(h, "tg_admin_pwd", hash, &len);
    nvs_close(h);

    return salt_err == ESP_OK && hash_err == ESP_OK &&
           strlen(salt) == TECH_SALT_HEX_LEN &&
           strlen(hash) == TECH_HASH_HEX_LEN;
}

bool config_has_telegram_admin_credentials(void)
{
    char name[CONFIG_TECH_NAME_MAX_LEN + 1] = {0};
    if (config_get_telegram_admin_name(name, sizeof(name)) != ESP_OK) {
        return false;
    }
    return name[0] != '\0' && config_has_telegram_admin_password();
}

esp_err_t config_set_telegram_admin_password(const char *password)
{
    if (!valid_password(password)) {
        return ESP_ERR_INVALID_ARG;
    }

    char salt[TECH_SALT_HEX_LEN + 1] = {0};
    char hash[TECH_HASH_HEX_LEN + 1] = {0};
    random_hex(salt, sizeof(salt), TECH_SALT_HEX_LEN / 2);
    ESP_RETURN_ON_ERROR(password_hash_hex(salt, password, hash, sizeof(hash)), TAG, "hash");

    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(SECRETS_NS, NVS_READWRITE, &h), TAG, "secrets open");
    esp_err_t err = nvs_set_str(h, "tg_admin_salt", salt);
    if (err == ESP_OK) {
        err = nvs_set_str(h, "tg_admin_pwd", hash);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t config_check_telegram_admin_credentials(const char *name, const char *password, bool *match)
{
    if (match) {
        *match = false;
    }
    if (!name || !password || !match) {
        return ESP_ERR_INVALID_ARG;
    }

    char stored_name[CONFIG_TECH_NAME_MAX_LEN + 1] = {0};
    ESP_RETURN_ON_ERROR(config_get_telegram_admin_name(stored_name, sizeof(stored_name)), TAG, "admin name");
    if (stored_name[0] == '\0' || strcmp(stored_name, name) != 0) {
        return ESP_OK;
    }

    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(SECRETS_NS, NVS_READONLY, &h), TAG, "secrets open");

    char salt[TECH_SALT_HEX_LEN + 1] = {0};
    char stored_hash[TECH_HASH_HEX_LEN + 1] = {0};
    size_t len = sizeof(salt);
    esp_err_t err = nvs_get_str(h, "tg_admin_salt", salt, &len);
    if (err == ESP_OK) {
        len = sizeof(stored_hash);
        err = nvs_get_str(h, "tg_admin_pwd", stored_hash, &len);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        return err;
    }

    char candidate[TECH_HASH_HEX_LEN + 1] = {0};
    ESP_RETURN_ON_ERROR(password_hash_hex(salt, password, candidate, sizeof(candidate)), TAG, "hash");
    *match = ct_equal_hex(candidate, stored_hash, TECH_HASH_HEX_LEN);
    return ESP_OK;
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

#ifndef CONFIG_IDMS_MQTT_URI_DEFAULT
#define CONFIG_IDMS_MQTT_URI_DEFAULT "mqtt://broker.hivemq.com:1883"
#endif

#ifndef CONFIG_IDMS_MODBUS_SLAVE_ID_DEFAULT
#define CONFIG_IDMS_MODBUS_SLAVE_ID_DEFAULT 1
#endif

esp_err_t config_get_mqtt_uri(char *out, size_t out_len)
{
    return secrets_get("mqtt_uri", CONFIG_IDMS_MQTT_URI_DEFAULT, out, out_len);
}

esp_err_t config_set_mqtt_uri(const char *uri)
{
    return secrets_set("mqtt_uri", uri);
}

esp_err_t config_get_mqtt_user(char *out, size_t out_len)
{
    return secrets_get("mqtt_user", "", out, out_len);
}

esp_err_t config_set_mqtt_user(const char *user)
{
    return secrets_set("mqtt_user", user);
}

esp_err_t config_get_mqtt_pass(char *out, size_t out_len)
{
    return secrets_get("mqtt_pass", "", out, out_len);
}

esp_err_t config_set_mqtt_pass(const char *pass)
{
    return secrets_set("mqtt_pass", pass);
}

esp_err_t config_get_mqtt_client_id(char *out, size_t out_len)
{
    char default_id[32];
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(default_id, sizeof(default_id), "idms_%02x%02x%02x", mac[3], mac[4], mac[5]);
    return secrets_get("mqtt_cid", default_id, out, out_len);
}

esp_err_t config_set_mqtt_client_id(const char *id)
{
    return secrets_set("mqtt_cid", id);
}

esp_err_t config_get_mb_slave_id(uint8_t *id)
{
    if (!id) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        *id = CONFIG_IDMS_MODBUS_SLAVE_ID_DEFAULT;
        return ESP_OK;
    }
    esp_err_t err = nvs_get_u8(h, "mb_id", id);
    nvs_close(h);
    if (err != ESP_OK) {
        *id = CONFIG_IDMS_MODBUS_SLAVE_ID_DEFAULT;
        return ESP_OK;
    }
    return ESP_OK;
}

esp_err_t config_set_mb_slave_id(uint8_t id)
{
    if (id < 1 || id > 247) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NS, NVS_READWRITE, &h), TAG, "open");
    esp_err_t err = nvs_set_u8(h, "mb_id", id);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/* ------------------------------------------------------------------ */
/*  Device/Telegram display strings stored in NVS "idms" namespace     */
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

int16_t config_get_tin_offset_x10(void)
{
    return nvs_get_i16_or_default("tin_off_x10", CONFIG_DEFAULT_TEMP_OFFSET_X10);
}

esp_err_t config_set_tin_offset_x10(int16_t value)
{
    if (value < CONFIG_TEMP_OFFSET_X10_MIN || value > CONFIG_TEMP_OFFSET_X10_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    return store_set_i16("tin_off_x10", value);
}

int16_t config_get_tout_offset_x10(void)
{
    return nvs_get_i16_or_default("tout_off_x10", CONFIG_DEFAULT_TEMP_OFFSET_X10);
}

esp_err_t config_set_tout_offset_x10(int16_t value)
{
    if (value < CONFIG_TEMP_OFFSET_X10_MIN || value > CONFIG_TEMP_OFFSET_X10_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    return store_set_i16("tout_off_x10", value);
}
