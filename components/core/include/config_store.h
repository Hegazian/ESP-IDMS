#pragma once

#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

esp_err_t config_store_init(void);

uint8_t config_get_tech_count(void);
esp_err_t config_get_tech_id(int idx, char *out, size_t out_len);
esp_err_t config_set_tech_id(int idx, const char *id);
esp_err_t config_add_tech_id(const char *id);

/**
 * Secrets stored in NVS namespace "secrets".
 * Each getter checks NVS first; if not set, falls back to the Kconfig default.
 * Each setter writes to NVS and persists across reboots.
 */
esp_err_t config_get_wifi_ssid(char *out, size_t out_len);
esp_err_t config_set_wifi_ssid(const char *ssid);
esp_err_t config_get_wifi_password(char *out, size_t out_len);
esp_err_t config_set_wifi_password(const char *password);
esp_err_t config_get_telegram_token(char *out, size_t out_len);
esp_err_t config_set_telegram_token(const char *token);
esp_err_t config_get_ota_user(char *out, size_t out_len);
esp_err_t config_set_ota_user(const char *user);
esp_err_t config_get_ota_pass(char *out, size_t out_len);
esp_err_t config_set_ota_pass(const char *pass);
