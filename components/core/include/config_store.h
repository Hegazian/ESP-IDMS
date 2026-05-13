#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define CONFIG_TECH_MAX_COUNT        5
#define CONFIG_TECH_NAME_MAX_LEN     32
#define CONFIG_TECH_PASSWORD_MAX_LEN 64

esp_err_t config_store_init(void);

uint8_t config_get_tech_count(void);
esp_err_t config_get_tech_id(int idx, char *out, size_t out_len);
esp_err_t config_set_tech_id(int idx, const char *id);
esp_err_t config_add_tech_id(const char *id);
esp_err_t config_find_tech_id(const char *id, int *idx_out);
esp_err_t config_get_tech_name(int idx, char *out, size_t out_len);
esp_err_t config_set_tech_name(int idx, const char *name);
esp_err_t config_get_tech_phone(int idx, char *out, size_t out_len);
esp_err_t config_set_tech_phone(int idx, const char *phone);
esp_err_t config_find_tech_phone(const char *phone, int *idx_out);
esp_err_t config_normalize_egypt_phone(const char *input, char *out, size_t out_len);
esp_err_t config_add_pending_tech_phone(const char *phone, const char *name);
esp_err_t config_bind_tech_phone(const char *phone, const char *telegram_id, const char *name);
esp_err_t config_add_tech(const char *id, const char *name);
esp_err_t config_remove_tech(int idx);
esp_err_t config_clear_techs(void);

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
esp_err_t config_get_telegram_admin_name(char *out, size_t out_len);
esp_err_t config_set_telegram_admin_name(const char *name);
bool config_has_telegram_admin_password(void);
bool config_has_telegram_admin_credentials(void);
esp_err_t config_set_telegram_admin_password(const char *password);
esp_err_t config_check_telegram_admin_credentials(const char *name, const char *password, bool *match);
esp_err_t config_get_ota_user(char *out, size_t out_len);
esp_err_t config_set_ota_user(const char *user);
esp_err_t config_get_ota_pass(char *out, size_t out_len);
esp_err_t config_set_ota_pass(const char *pass);
esp_err_t config_get_cloud_url(char *out, size_t out_len);
esp_err_t config_set_cloud_url(const char *url);
esp_err_t config_get_cloud_token(char *out, size_t out_len);
esp_err_t config_set_cloud_token(const char *token);

/* ------------------------------------------------------------------ */
/*  Device/Telegram display strings shown on the Topway pages          */
/*  Stored in NVS namespace "idms" and persisted across reboots.       */
/* ------------------------------------------------------------------ */

#define CONFIG_INFO_STRING_MAX_LEN       127
#define CONFIG_DEFAULT_DEVICE_MODEL      "ESP-IDMS"
#define CONFIG_DEFAULT_SUPPORT_EMAIL     ""
#define CONFIG_DEFAULT_SUPPORT_PHONE     ""
#define CONFIG_DEFAULT_QR_CODE           "https://t.me/IDMS_USERBOT"

esp_err_t config_get_device_model(char *out, size_t out_len);
esp_err_t config_set_device_model(const char *value);
esp_err_t config_get_serial_number(char *out, size_t out_len);
esp_err_t config_set_serial_number(const char *value);
esp_err_t config_get_manufacture_date(char *out, size_t out_len);
esp_err_t config_set_manufacture_date(const char *value);
esp_err_t config_get_support_email(char *out, size_t out_len);
esp_err_t config_set_support_email(const char *value);
esp_err_t config_get_support_phone(char *out, size_t out_len);
esp_err_t config_set_support_phone(const char *value);
esp_err_t config_get_qr_code(char *out, size_t out_len);
esp_err_t config_set_qr_code(const char *value);

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

/* Sensor calibration offsets stored as Celsius x10. */
#define CONFIG_TEMP_OFFSET_X10_MIN       -200  /* -20.0 C */
#define CONFIG_TEMP_OFFSET_X10_MAX        200  /* +20.0 C */
#define CONFIG_DEFAULT_TEMP_OFFSET_X10    0

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
uint16_t config_get_power_loss_current_ma(void);
esp_err_t config_set_power_loss_current_ma(uint16_t value);
uint16_t config_get_machine_running_current_ma(void);
esp_err_t config_set_machine_running_current_ma(uint16_t value);
int16_t config_get_dt_alert_threshold(void);
esp_err_t config_set_dt_alert_threshold(int16_t value);
int16_t config_get_dt_high_threshold(void);
esp_err_t config_set_dt_high_threshold(int16_t value);
uint32_t config_get_current_cal_x100(void);
esp_err_t config_set_current_cal_x100(uint32_t value);
int16_t config_get_tin_offset_x10(void);
esp_err_t config_set_tin_offset_x10(int16_t value);
int16_t config_get_tout_offset_x10(void);
esp_err_t config_set_tout_offset_x10(int16_t value);
