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

/* ------------------------------------------------------------------ */
/*  Threshold configuration for temperature and current limits         */
/*  Stored in NVS with defaults. Pure Celsius and Amperes             */
/* ------------------------------------------------------------------ */

/* Temperature limits in degrees C (pure values, no multiplier) */
#define CONFIG_TEMP_MIN_LIMIT   -50     /* -50 C */
#define CONFIG_TEMP_MAX_LIMIT    100    /* 100 C */

/* Current limits in Amperes (pure values, no multiplier) */
#define CONFIG_CURRENT_MIN_LIMIT     0      /* 0 A */
#define CONFIG_CURRENT_MAX_LIMIT     30     /* 30 A */

/* Default values */
#define CONFIG_DEFAULT_MIN_TIN       -10    /* -10 C */
#define CONFIG_DEFAULT_MAX_TIN       0      /* 0 C */
#define CONFIG_DEFAULT_MIN_TOUT      0      /* 0 C */
#define CONFIG_DEFAULT_MAX_TOUT      55     /* 55 C */
#define CONFIG_DEFAULT_MIN_CURRENT   1      /* 1 A */
#define CONFIG_DEFAULT_MAX_CURRENT   20     /* 20 A */

/* Delta temperature alert threshold in C (pure value) */
#define CONFIG_DEFAULT_DT_ALERT_THRESHOLD  5   /* 5 C */

int16_t config_get_min_tin(void);
esp_err_t config_set_min_tin(int16_t value);
int16_t config_get_max_tin(void);
esp_err_t config_set_max_tin(int16_t value);
int16_t config_get_min_tout(void);
esp_err_t config_set_min_tout(int16_t value);
int16_t config_get_max_tout(void);
esp_err_t config_set_max_tout(int16_t value);
uint16_t config_get_min_current(void);
esp_err_t config_set_min_current(uint16_t value);
uint16_t config_get_max_current(void);
esp_err_t config_set_max_current(uint16_t value);
int16_t config_get_dt_alert_threshold(void);
esp_err_t config_set_dt_alert_threshold(int16_t value);

