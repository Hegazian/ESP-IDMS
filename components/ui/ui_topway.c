#include "sdkconfig.h"
#ifndef CONFIG_IDMS_PIN_UART_RTS
#define CONFIG_IDMS_PIN_UART_RTS -1
#endif
#ifndef CONFIG_IDMS_TOPWAY_BAUD
#define CONFIG_IDMS_TOPWAY_BAUD 9600
#endif

#include "ui_topway.h"
#include "topway_lcd.h"
#include "monitor.h"
#include "config_store.h"
#include "network_manager.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *TAG = "ui_topway";

#define PAGE_MAIN 0
#define INFO_FIELD_MAX_LEN (CONFIG_INFO_STRING_MAX_LEN + 1)

static TaskHandle_t s_ui_task = NULL;
static bool s_display_ready = false;
static float s_last_current = -999.0f;
static float s_last_tin = -999.0f;
static float s_last_tout = -999.0f;
static char s_last_diag[sizeof(((idms_metrics_t *)0)->sensor_status)] = "";
static uint16_t s_last_apply_btn = 0;  /* Track apply button state */
static uint32_t s_last_telegram_panel_update = 0;

/* Runtime config values - can be modified via LCD */
static int16_t  s_cfg_min_tin = 0;
static int16_t  s_cfg_min_tout = 0;
static uint16_t s_cfg_min_current = 0;
static int16_t  s_cfg_max_tin = 0;
static int16_t  s_cfg_max_tout = 0;
static uint16_t s_cfg_max_current = 0;
static int16_t  s_cfg_dt_alert = 0;
static bool     s_cfg_applied = false;

/* WiFi status tracking */
static bool s_last_wifi_connected = false;
static uint32_t s_wifi_status_last_update = 0;
static uint32_t s_wifi_last_connected_tick = 0;
#define WIFI_STATUS_UPDATE_INTERVAL_MS 5000  /* Update every 5 seconds max */
#define WIFI_CONNECTION_TIMEOUT_MS 30000     /* 30 seconds to establish connection */

/* One-time WiFi field clear flag (workaround for LCD auto-refresh) */
static bool s_wifi_fields_cleared = false;

/* WiFi button cooldown to prevent rapid re-triggering */
static uint32_t s_wifi_btn_last_trigger = 0;
#define WIFI_BTN_COOLDOWN_MS 10000 /* 10 second cooldown between button presses */
#define UI_TOPWAY_REFRESH_MS 500
#define UI_TOPWAY_CONTROL_POLL_MS 1000
#define UI_TOPWAY_CONTROL_BACKOFF_MS 5000
#define UI_TOPWAY_TELEGRAM_POLL_MS 5000
#define UI_TOPWAY_RTC_UPDATE_MS 30000
#define UI_TOPWAY_PANEL_RTC_SYNC_MS (60 * 60 * 1000)
#define UI_TOPWAY_RECONNECT_MS 3000
#define UI_TOPWAY_BOOT_SETTLE_MS 750
#define UI_TOPWAY_POST_CONNECT_RESYNC_MS 3000
#define UI_TOPWAY_HEALTHCHECK_MS 5000
#ifndef CONFIG_IDMS_TOPWAY_FULL_REFRESH_MS
#define CONFIG_IDMS_TOPWAY_FULL_REFRESH_MS 60000
#endif
#define UI_TOPWAY_PERIODIC_RESYNC_MS CONFIG_IDMS_TOPWAY_FULL_REFRESH_MS
#define UI_TOPWAY_MAX_IO_FAILURES 3
#define TOPWAY_WRITE_CACHE_MAX 48
#define TOPWAY_WRITE_STR_CACHE_LEN 96
#define TOPWAY_INVALID_READING_N16 ((uint16_t)0xFC19)  /* -999 when N16 is signed */
#define TZ_AFRICA_CAIRO "EET-2EEST,M4.5.5/0,M10.5.4/24"

typedef enum {
  TOPWAY_CACHE_EMPTY = 0,
  TOPWAY_CACHE_N16,
  TOPWAY_CACHE_STR,
} topway_cache_type_t;

typedef struct {
  uint32_t vp;
  topway_cache_type_t type;
  bool valid;
  uint16_t n16;
  char str[TOPWAY_WRITE_STR_CACHE_LEN];
} topway_write_cache_t;

static topway_write_cache_t s_write_cache[TOPWAY_WRITE_CACHE_MAX];
static uint32_t s_last_control_poll = 0;
static uint32_t s_control_backoff_until = 0;
static uint32_t s_last_rtc_update = 0;
static uint32_t s_last_panel_rtc_sync = 0;
static uint32_t s_last_reconnect_attempt = 0;
static uint32_t s_last_healthcheck = 0;
static uint32_t s_last_periodic_resync = 0;
static uint32_t s_full_resync_due = 0;
static bool s_full_resync_pending = false;
static int s_topway_io_failures = 0;

static uint16_t display_round_n16(float value);
static uint16_t status_color_for_state(uint16_t device_state);
static void topway_invalidate_write_cache(void);
static void topway_note_io_result(esp_err_t err);
static esp_err_t topway_merge_err(esp_err_t current, esp_err_t next);
static esp_err_t topway_n16_write_checked(uint32_t vp, uint16_t value);
static esp_err_t topway_str_write_checked(uint32_t vp, const char *value);
static esp_err_t update_display_values(void);
static esp_err_t update_telegram_panel(bool force);
static esp_err_t send_device_info_to_topway(bool reset_controls);
static esp_err_t send_config_to_topway(void);
static esp_err_t send_calibration_to_topway(void);
static esp_err_t topway_connect_sequence(void);

static uint8_t topway_baud_code_from_rate(uint32_t baud_rate)
{
  switch (baud_rate) {
    case 1200:
      return TOPWAY_BAUD_1200;
    case 2400:
      return TOPWAY_BAUD_2400;
    case 4800:
      return TOPWAY_BAUD_4800;
    case 9600:
      return TOPWAY_BAUD_9600;
    case 19200:
      return TOPWAY_BAUD_19200;
    case 38400:
      return TOPWAY_BAUD_38400;
    case 57600:
      return TOPWAY_BAUD_57600;
    case 115200:
      return TOPWAY_BAUD_115200;
    default:
      ESP_LOGW(TAG, "Unsupported Topway baud %lu, using 115200 for panel config",
               (unsigned long)baud_rate);
      return TOPWAY_BAUD_115200;
  }
}

static topway_write_cache_t *find_write_cache(uint32_t vp, topway_cache_type_t type)
{
  topway_write_cache_t *free_slot = NULL;
  for (size_t i = 0; i < TOPWAY_WRITE_CACHE_MAX; i++) {
    if (s_write_cache[i].type == type && s_write_cache[i].vp == vp) {
      return &s_write_cache[i];
    }
    if (!free_slot && s_write_cache[i].type == TOPWAY_CACHE_EMPTY) {
      free_slot = &s_write_cache[i];
    }
  }

  topway_write_cache_t *slot = free_slot ? free_slot : &s_write_cache[0];
  memset(slot, 0, sizeof(*slot));
  slot->vp = vp;
  slot->type = type;
  return slot;
}

static void topway_invalidate_write_cache(void)
{
  memset(s_write_cache, 0, sizeof(s_write_cache));
}

static void topway_mark_not_ready(const char *reason, esp_err_t err)
{
  if (s_display_ready) {
    ESP_LOGW(TAG, "Topway link lost (%s: %s); will retry handshake",
             reason ? reason : "I/O", esp_err_to_name(err));
  }
  s_display_ready = false;
  s_topway_io_failures = 0;
  s_control_backoff_until = 0;
  s_last_reconnect_attempt = 0;
  s_wifi_fields_cleared = false;
  s_full_resync_pending = false;
  topway_invalidate_write_cache();
}

static void topway_note_io_result(esp_err_t err)
{
  if (err == ESP_OK) {
    s_topway_io_failures = 0;
    return;
  }

  if (err == ESP_ERR_TIMEOUT || err == ESP_ERR_INVALID_RESPONSE ||
      err == ESP_ERR_INVALID_STATE) {
    s_topway_io_failures++;
    if (s_display_ready && s_topway_io_failures >= UI_TOPWAY_MAX_IO_FAILURES) {
      topway_mark_not_ready("I/O failures", err);
    }
  }
}

static esp_err_t topway_merge_err(esp_err_t current, esp_err_t next)
{
  return current == ESP_OK ? next : current;
}

static void topway_cache_n16_value(uint32_t vp, uint16_t value)
{
  topway_write_cache_t *cache = find_write_cache(vp, TOPWAY_CACHE_N16);
  cache->n16 = value;
  cache->valid = true;
}

static void topway_cache_str_value(uint32_t vp, const char *value)
{
  topway_write_cache_t *cache = find_write_cache(vp, TOPWAY_CACHE_STR);
  strncpy(cache->str, value ? value : "", sizeof(cache->str) - 1);
  cache->str[sizeof(cache->str) - 1] = '\0';
  cache->valid = true;
}

static esp_err_t topway_n16_write_checked(uint32_t vp, uint16_t value)
{
  esp_err_t err = topway_n16_write(vp, value);
  topway_note_io_result(err);
  if (err == ESP_OK) {
    topway_cache_n16_value(vp, value);
  }
  return err;
}

static esp_err_t topway_str_write_checked(uint32_t vp, const char *value)
{
  if (!value) {
    value = "";
  }
  esp_err_t err = topway_str_write(vp, value);
  topway_note_io_result(err);
  if (err == ESP_OK) {
    topway_cache_str_value(vp, value);
  }
  return err;
}

static esp_err_t topway_n16_write_cached(uint32_t vp, uint16_t value)
{
  topway_write_cache_t *cache = find_write_cache(vp, TOPWAY_CACHE_N16);
  if (cache->valid && cache->n16 == value) {
    return ESP_OK;
  }
  esp_err_t err = topway_n16_write(vp, value);
  topway_note_io_result(err);
  if (err == ESP_OK) {
    topway_cache_n16_value(vp, value);
  }
  return err;
}

static esp_err_t topway_str_write_cached(uint32_t vp, const char *value)
{
  if (!value) {
    value = "";
  }
  topway_write_cache_t *cache = find_write_cache(vp, TOPWAY_CACHE_STR);
  if (cache->valid && strcmp(cache->str, value) == 0) {
    return ESP_OK;
  }
  esp_err_t err = topway_str_write(vp, value);
  topway_note_io_result(err);
  if (err == ESP_OK) {
    topway_cache_str_value(vp, value);
  }
  return err;
}

static esp_err_t topway_control_n16_read(uint32_t vp, uint16_t *value)
{
  if (topway_take_touch_event(vp, value)) {
    return ESP_OK;
  }

  esp_err_t err = topway_n16_read(vp, value);
  if (err != ESP_OK) {
    s_control_backoff_until = xTaskGetTickCount() * portTICK_PERIOD_MS + UI_TOPWAY_CONTROL_BACKOFF_MS;
  }
  return err;
}

static bool topway_controls_in_backoff(void)
{
  return (xTaskGetTickCount() * portTICK_PERIOD_MS) < s_control_backoff_until;
}

static void copy_info_cache(char *dst, const char *src)
{
  if (!dst) {
    return;
  }
  if (!src) {
    src = "";
  }
  strncpy(dst, src, INFO_FIELD_MAX_LEN - 1);
  dst[INFO_FIELD_MAX_LEN - 1] = '\0';
}

static esp_err_t update_wifi_status_on_lcd(bool force)
{
  if (!s_display_ready)
    return ESP_ERR_INVALID_STATE;

  uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
  bool is_connected = network_manager_is_connected();

  /* Track when we were last connected */
  if (is_connected) {
    s_wifi_last_connected_tick = now;
  }

  /* Only update when state changes or on interval */
  if (!force &&
      is_connected == s_last_wifi_connected &&
      (now - s_wifi_status_last_update) < WIFI_STATUS_UPDATE_INTERVAL_MS) {
    return ESP_OK;
  }

  s_last_wifi_connected = is_connected;
  s_wifi_status_last_update = now;
  esp_err_t result = ESP_OK;

  if (is_connected) {
    /* Connected - show IP without touching editable SSID/password fields */
    char ip[16] = {0};
    network_manager_get_ip(ip, sizeof(ip));

    char status_msg[32];
    snprintf(status_msg, sizeof(status_msg), "WiFi: %s", ip);
    result = topway_str_write_cached(VP_STR_WIFI_STATUS_MSG, status_msg);
  } else {
    /* Disconnected - check if credentials are configured */
    char ssid[64] = {0};
    config_get_wifi_ssid(ssid, sizeof(ssid));

    if (ssid[0] == '\0') {
      result = topway_str_write_cached(VP_STR_WIFI_STATUS_MSG, "WiFi: Not Configured");
    } else if ((now - s_wifi_last_connected_tick) < WIFI_CONNECTION_TIMEOUT_MS) {
      /* Recently disconnected, might be reconnecting */
      result = topway_str_write_cached(VP_STR_WIFI_STATUS_MSG, "WiFi: Connecting...");
    } else {
      /* Connection failed or timeout */
      result = topway_str_write_cached(VP_STR_WIFI_STATUS_MSG, "WiFi: Check Password");
    }
  }

  return result;
}

static bool esp_rtc_time_valid(time_t now)
{
  return now > 1704067200; /* 2024-01-01 UTC: rejects unsynced boot epoch */
}

static esp_err_t update_rtc_on_lcd(bool force)
{
  if (!s_display_ready) {
    return ESP_ERR_INVALID_STATE;
  }

  uint32_t tick_now = xTaskGetTickCount() * portTICK_PERIOD_MS;
  if (!force && (tick_now - s_last_rtc_update) < UI_TOPWAY_RTC_UPDATE_MS) {
    return ESP_OK;
  }
  s_last_rtc_update = tick_now;

  time_t now = time(NULL);
  if (!esp_rtc_time_valid(now)) {
    return topway_str_write_cached(VP_STR_RTC_DATETIME, "Time not synced");
  }

  struct tm local_tm;
  if (localtime_r(&now, &local_tm) == NULL) {
    return topway_str_write_cached(VP_STR_RTC_DATETIME, "Time unavailable");
  }

  char dt[32];
  strftime(dt, sizeof(dt), "%Y-%m-%d %H:%M", &local_tm);
  esp_err_t result = topway_str_write_cached(VP_STR_RTC_DATETIME, dt);

  if (force || (tick_now - s_last_panel_rtc_sync) >= UI_TOPWAY_PANEL_RTC_SYNC_MS) {
    esp_err_t err = topway_rtc_set((uint8_t)(local_tm.tm_year % 100),
                                   (uint8_t)(local_tm.tm_mon + 1),
                                   (uint8_t)local_tm.tm_mday,
                                   (uint8_t)local_tm.tm_hour,
                                   (uint8_t)local_tm.tm_min,
                                   (uint8_t)local_tm.tm_sec);
    if (err == ESP_OK) {
      s_last_panel_rtc_sync = tick_now;
    } else {
      ESP_LOGW(TAG, "Topway RTC sync failed: %s", esp_err_to_name(err));
    }
    result = topway_merge_err(result, err);
  }
  return result;
}

static esp_err_t update_display_values(void) {
  if (!s_display_ready)
    return ESP_ERR_INVALID_STATE;

  esp_err_t result = ESP_OK;

  idms_metrics_t m;
  monitor_get_metrics(&m);

  float current_rounded = m.current_valid ? (float)(int)(m.current_a + 0.5f) : -999.0f;
  float tin_rounded = m.t_in_valid ? (float)(int)(m.t_in_c >= 0.0f ? m.t_in_c + 0.5f : m.t_in_c - 0.5f) : -999.0f;
  float tout_rounded = m.t_out_valid ? (float)(int)(m.t_out_c >= 0.0f ? m.t_out_c + 0.5f : m.t_out_c - 0.5f) : -999.0f;

  if (m.current_valid) {
    result = topway_merge_err(result, topway_n16_write_cached(VP_N16_CUR_VALUE, display_round_n16(m.current_a)));
    if (current_rounded != s_last_current) {
      ESP_LOGI(TAG, "Current=%.0fA", current_rounded);
      s_last_current = current_rounded;
    }
  } else {
    result = topway_merge_err(result, topway_n16_write_cached(VP_N16_CUR_VALUE, TOPWAY_INVALID_READING_N16));
    if (s_last_current != -888.0f) {
      ESP_LOGI(TAG, "Current=-- (invalid)");
      s_last_current = -888.0f;
    }
  }

  if (m.t_in_valid) {
    result = topway_merge_err(result, topway_n16_write_cached(VP_N16_TIN_VALUE, display_round_n16(m.t_in_c)));
    if (tin_rounded != s_last_tin) {
      ESP_LOGI(TAG, "T_in=%.0fC", tin_rounded);
      s_last_tin = tin_rounded;
    }
  } else {
    result = topway_merge_err(result, topway_n16_write_cached(VP_N16_TIN_VALUE, TOPWAY_INVALID_READING_N16));
    if (s_last_tin != -888.0f) {
      ESP_LOGI(TAG, "T_in=-- (invalid)");
      s_last_tin = -888.0f;
    }
  }

  if (m.t_out_valid) {
    result = topway_merge_err(result, topway_n16_write_cached(VP_N16_TOUT_VALUE, display_round_n16(m.t_out_c)));
    if (tout_rounded != s_last_tout) {
      ESP_LOGI(TAG, "T_out=%.0fC", tout_rounded);
      s_last_tout = tout_rounded;
    }
  } else {
    result = topway_merge_err(result, topway_n16_write_cached(VP_N16_TOUT_VALUE, TOPWAY_INVALID_READING_N16));
    if (s_last_tout != -888.0f) {
      ESP_LOGI(TAG, "T_out=-- (invalid)");
      s_last_tout = -888.0f;
    }
  }

  /* Determine device status and diagnostic message */
  const char *status_str = "ACTIVE";
  const char *diag_str = "OK";
  uint16_t device_state = DEVICE_STATE_ACTIVE;

  bool has_power_fault = m.power_fault;
  bool has_cool_fault = m.cooling_fault;
  bool temp_invalid = (!m.t_in_valid || !m.t_out_valid);
  bool current_invalid = !m.current_valid;

  if (m.sensor_preflight_done && !m.sensor_preflight_ok) {
    status_str = "ERROR";
    diag_str = m.sensor_status[0] ? m.sensor_status : "Sensor Preflight";
    device_state = DEVICE_STATE_ERROR;
  } else if (temp_invalid && current_invalid) {
    status_str = "ERROR";
    diag_str = "Sensors Offline";
    device_state = DEVICE_STATE_ERROR;
  } else if (has_power_fault && has_cool_fault) {
    status_str = "ERROR";
    diag_str = "Power + Cooling";
    device_state = DEVICE_STATE_ERROR;
  } else if (has_power_fault) {
    status_str = "ERROR";
    diag_str = "Power Loss";
    device_state = DEVICE_STATE_ERROR;
  } else if (has_cool_fault) {
    status_str = "ERROR";
    diag_str = "Cooling Fault";
    device_state = DEVICE_STATE_ERROR;
  } else if (temp_invalid) {
    status_str = "WARNING";
    diag_str = "Temp Sensor";
    device_state = DEVICE_STATE_WARNING;
  } else if (current_invalid) {
    status_str = "WARNING";
    diag_str = "Current Sensor";
    device_state = DEVICE_STATE_WARNING;
  } else if (m.current_valid && m.current_a < 0.5f) {
    status_str = "INACTIVE";
    diag_str = "Standby";
    device_state = DEVICE_STATE_INACTIVE;
  }

  result = topway_merge_err(result, topway_n16_write_cached(VP_N16_STATUS_COLOR, status_color_for_state(device_state)));
  result = topway_merge_err(result, topway_str_write_cached(VP_STR_STATUS, status_str));
  result = topway_merge_err(result, topway_str_write_cached(VP_STR_DIAG, diag_str));
  if (strcmp(diag_str, s_last_diag) != 0) {
    ESP_LOGI(TAG, "Status=%s Diag=%s", status_str, diag_str);
    strncpy(s_last_diag, diag_str, sizeof(s_last_diag) - 1);
    s_last_diag[sizeof(s_last_diag) - 1] = '\0';
  }

  return result;
}

static bool validate_lcd_config(int16_t min_tin, int16_t max_tin,
                                int16_t min_tout, int16_t max_tout,
                                uint16_t min_current, uint16_t max_current,
                                int16_t dt_alert)
{
  if (min_tin < CONFIG_TEMP_MIN_LIMIT || min_tin > CONFIG_TEMP_MAX_LIMIT) return false;
  if (max_tin < CONFIG_TEMP_MIN_LIMIT || max_tin > CONFIG_TEMP_MAX_LIMIT) return false;
  if (min_tout < CONFIG_TEMP_MIN_LIMIT || min_tout > CONFIG_TEMP_MAX_LIMIT) return false;
  if (max_tout < CONFIG_TEMP_MIN_LIMIT || max_tout > CONFIG_TEMP_MAX_LIMIT) return false;
  if (min_current > CONFIG_CURRENT_MAX_LIMIT) return false;
  if (max_current > CONFIG_CURRENT_MAX_LIMIT) return false;
  if (dt_alert < 0 || dt_alert > 100) return false;
  if (min_tin > max_tin) return false;
  if (min_tout > max_tout) return false;
  if (min_current > max_current) return false;
  return true;
}

static esp_err_t read_lcd_config(int16_t *min_tin, int16_t *max_tin,
                                 int16_t *min_tout, int16_t *max_tout,
                                 uint16_t *min_current, uint16_t *max_current,
                                 int16_t *dt_alert)
{
  uint16_t val = 0;
  esp_err_t err = topway_n16_read(VP_N16_CFG_MIN_TIN, &val);
  if (err != ESP_OK) return err;
  *min_tin = (int16_t)val;

  err = topway_n16_read(VP_N16_CFG_MAX_TIN, &val);
  if (err != ESP_OK) return err;
  *max_tin = (int16_t)val;

  err = topway_n16_read(VP_N16_CFG_MIN_TOUT, &val);
  if (err != ESP_OK) return err;
  *min_tout = (int16_t)val;

  err = topway_n16_read(VP_N16_CFG_MAX_TOUT, &val);
  if (err != ESP_OK) return err;
  *max_tout = (int16_t)val;

  err = topway_n16_read(VP_N16_CFG_MIN_CURRENT, &val);
  if (err != ESP_OK) return err;
  *min_current = val;

  err = topway_n16_read(VP_N16_CFG_MAX_CURRENT, &val);
  if (err != ESP_OK) return err;
  *max_current = val;

  err = topway_n16_read(VP_N16_CFG_DT_ALERT, &val);
  if (err != ESP_OK) return err;
  *dt_alert = (int16_t)val;

  return ESP_OK;
}

static esp_err_t save_lcd_config(int16_t min_tin, int16_t max_tin,
                                 int16_t min_tout, int16_t max_tout,
                                 uint16_t min_current, uint16_t max_current,
                                 int16_t dt_alert)
{
  esp_err_t err = ESP_OK;
  if (config_set_min_tin(min_tin) != ESP_OK) err = ESP_FAIL;
  if (config_set_max_tin(max_tin) != ESP_OK) err = ESP_FAIL;
  if (config_set_min_tout(min_tout) != ESP_OK) err = ESP_FAIL;
  if (config_set_max_tout(max_tout) != ESP_OK) err = ESP_FAIL;
  if (config_set_min_current(min_current) != ESP_OK) err = ESP_FAIL;
  if (config_set_max_current(max_current) != ESP_OK) err = ESP_FAIL;
  if (config_set_dt_alert_threshold(dt_alert) != ESP_OK) err = ESP_FAIL;
  if (err != ESP_OK) return err;

  s_cfg_min_tin = min_tin;
  s_cfg_max_tin = max_tin;
  s_cfg_min_tout = min_tout;
  s_cfg_max_tout = max_tout;
  s_cfg_min_current = min_current;
  s_cfg_max_current = max_current;
  s_cfg_dt_alert = dt_alert;
  s_cfg_applied = true;

  ESP_LOGI(TAG, "Config saved to NVS: MinTin=%d, MaxTin=%d, MinTout=%d, MaxTout=%d, MinCur=%dA, MaxCur=%dA, DeltaT=%dC",
           s_cfg_min_tin, s_cfg_max_tin, s_cfg_min_tout, s_cfg_max_tout,
           s_cfg_min_current, s_cfg_max_current, s_cfg_dt_alert);
  return ESP_OK;
}

static void apply_lcd_config(bool clear_apply_button)
{
  int16_t min_tin = s_cfg_min_tin;
  int16_t max_tin = s_cfg_max_tin;
  int16_t min_tout = s_cfg_min_tout;
  int16_t max_tout = s_cfg_max_tout;
  uint16_t min_current = s_cfg_min_current;
  uint16_t max_current = s_cfg_max_current;
  int16_t dt_alert = s_cfg_dt_alert;

  esp_err_t err = read_lcd_config(&min_tin, &max_tin, &min_tout, &max_tout,
                                  &min_current, &max_current, &dt_alert);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Cannot apply LCD config; read failed: %s", esp_err_to_name(err));
    topway_str_write_cached(VP_STR_CFG_STATUS_MSG, "Config read failed");
    if (clear_apply_button) topway_n16_write(VP_N16_CFG_APPLY_BTN, 0);
    return;
  }

  if (!validate_lcd_config(min_tin, max_tin, min_tout, max_tout,
                           min_current, max_current, dt_alert)) {
    ESP_LOGW(TAG, "Config validation failed");
    topway_str_write_cached(VP_STR_CFG_STATUS_MSG, "Check min/max values");
    if (clear_apply_button) topway_n16_write(VP_N16_CFG_APPLY_BTN, 0);
    return;
  }

  err = save_lcd_config(min_tin, max_tin, min_tout, max_tout,
                        min_current, max_current, dt_alert);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to save config to NVS");
    topway_str_write_cached(VP_STR_CFG_STATUS_MSG, "Config save failed");
  } else {
    topway_str_write_cached(VP_STR_CFG_STATUS_MSG, "Config saved");
  }
  if (clear_apply_button) topway_n16_write(VP_N16_CFG_APPLY_BTN, 0);
}

static void trim_ascii_in_place(char *value)
{
  if (!value) {
    return;
  }
  char *start = value;
  while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') {
    start++;
  }
  if (start != value) {
    memmove(value, start, strlen(start) + 1);
  }
  size_t len = strlen(value);
  while (len > 0 &&
         (value[len - 1] == ' ' || value[len - 1] == '\t' ||
          value[len - 1] == '\r' || value[len - 1] == '\n')) {
    value[--len] = '\0';
  }
}

static uint16_t display_round_n16(float value)
{
  int32_t rounded = (int32_t)(value >= 0.0f ? value + 0.5f : value - 0.5f);
  if (rounded < INT16_MIN) {
    rounded = INT16_MIN;
  } else if (rounded > INT16_MAX) {
    rounded = INT16_MAX;
  }
  return (uint16_t)(int16_t)rounded;
}

static uint16_t status_color_for_state(uint16_t device_state)
{
  switch (device_state) {
    case DEVICE_STATE_ACTIVE:
      return TOPWAY_COLOR_GREEN;
    case DEVICE_STATE_ERROR:
      return TOPWAY_COLOR_RED;
    case DEVICE_STATE_WARNING:
      return TOPWAY_COLOR_YELLOW;
    default:
      return TOPWAY_COLOR_GRAY;
  }
}

static uint16_t current_cal_x100_to_vp(uint32_t cal_x100)
{
  uint32_t vp = (cal_x100 + 50) / 100;
  if (vp > 5000) {
    vp = 5000;
  }
  if (vp == 0) {
    vp = 1;
  }
  return (uint16_t)vp;
}

static uint16_t temp_offset_x10_to_vp(int16_t offset_x10)
{
  int16_t rounded = (offset_x10 >= 0) ?
                    (int16_t)((offset_x10 + 5) / 10) :
                    (int16_t)((offset_x10 - 5) / 10);
  return (uint16_t)rounded;
}

static esp_err_t read_lcd_calibration(uint32_t *current_cal_x100,
                                      int16_t *tin_offset_x10,
                                      int16_t *tout_offset_x10)
{
  uint16_t val = 0;
  esp_err_t err = topway_n16_read(VP_N16_CAL_CURRENT_SCALE, &val);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Calibration read failed: current scale VP 0x%06lX (%s)",
             (unsigned long)VP_N16_CAL_CURRENT_SCALE, esp_err_to_name(err));
    return err;
  }
  if (val == 0 || val > 5000) {
    ESP_LOGW(TAG, "Calibration read failed: current scale value %u out of range", val);
    return ESP_ERR_INVALID_ARG;
  }
  if (current_cal_x100) *current_cal_x100 = (uint32_t)val * 100;

  err = topway_n16_read(VP_N16_CAL_TIN_OFFSET, &val);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Calibration read failed: T_in offset VP 0x%06lX (%s)",
             (unsigned long)VP_N16_CAL_TIN_OFFSET, esp_err_to_name(err));
    return err;
  }
  if (tin_offset_x10) *tin_offset_x10 = (int16_t)val * 10;

  err = topway_n16_read(VP_N16_CAL_TOUT_OFFSET, &val);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Calibration read failed: T_out offset VP 0x%06lX (%s)",
             (unsigned long)VP_N16_CAL_TOUT_OFFSET, esp_err_to_name(err));
    return err;
  }
  if (tout_offset_x10) *tout_offset_x10 = (int16_t)val * 10;

  ESP_LOGI(TAG, "LCD calibration read: current=%.1f A/V, TinOffset=%.1f C, ToutOffset=%.1f C",
           current_cal_x100 ? (double)*current_cal_x100 / 100.0 : 0.0,
           tin_offset_x10 ? (double)*tin_offset_x10 / 10.0 : 0.0,
           tout_offset_x10 ? (double)*tout_offset_x10 / 10.0 : 0.0);
  return ESP_OK;
}

static esp_err_t save_lcd_calibration_values(uint32_t current_cal_x100,
                                             int16_t tin_offset_x10,
                                             int16_t tout_offset_x10)
{
  ESP_RETURN_ON_ERROR(config_set_current_cal_x100(current_cal_x100), TAG, "set current cal");
  ESP_RETURN_ON_ERROR(config_set_tin_offset_x10(tin_offset_x10), TAG, "set tin offset");
  ESP_RETURN_ON_ERROR(config_set_tout_offset_x10(tout_offset_x10), TAG, "set tout offset");
  monitor_reset_current_filter();
  return ESP_OK;
}

static void apply_lcd_calibration(bool clear_buttons)
{
  uint32_t current_cal_x100 = config_get_current_cal_x100();
  int16_t tin_offset_x10 = config_get_tin_offset_x10();
  int16_t tout_offset_x10 = config_get_tout_offset_x10();

  esp_err_t err = read_lcd_calibration(&current_cal_x100, &tin_offset_x10, &tout_offset_x10);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Cannot apply LCD calibration; read/validation failed: %s", esp_err_to_name(err));
    topway_str_write_cached(VP_STR_CAL_STATUS_MSG, "Calibration read failed");
    if (clear_buttons) {
      topway_n16_write(VP_N16_CAL_APPLY_BTN, 0);
      topway_n16_write(VP_N16_CAL_SAVE_BTN, 0);
    }
    return;
  }

  err = save_lcd_calibration_values(current_cal_x100, tin_offset_x10, tout_offset_x10);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Cannot save LCD calibration: %s", esp_err_to_name(err));
    topway_str_write_cached(VP_STR_CAL_STATUS_MSG, "Calibration invalid");
  } else {
    ESP_LOGI(TAG, "Calibration saved: current=%.1f A/V, TinOffset=%.1f C, ToutOffset=%.1f C",
             (double)current_cal_x100 / 100.0,
             (double)tin_offset_x10 / 10.0,
             (double)tout_offset_x10 / 10.0);
    topway_str_write_cached(VP_STR_CAL_STATUS_MSG, "Calibration saved");
  }

  if (clear_buttons) {
    topway_n16_write(VP_N16_CAL_APPLY_BTN, 0);
    topway_n16_write(VP_N16_CAL_SAVE_BTN, 0);
  }
}

static void zero_current_from_lcd(void)
{
#if CONFIG_IDMS_SCT_AUTOZERO_ENABLE
  topway_str_write_cached(VP_STR_CAL_STATUS_MSG, "Zeroing current...");
  ESP_LOGI(TAG, "LCD requested current zero calibration");
  esp_err_t err = monitor_calibrate_zero_manual();
  monitor_reset_current_filter();
  if (err == ESP_OK) {
    topway_str_write_cached(VP_STR_CAL_STATUS_MSG, "Current zero saved");
  } else {
    topway_str_write_cached(VP_STR_CAL_STATUS_MSG, "Current zero failed");
    ESP_LOGW(TAG, "LCD current zero calibration failed: %s", esp_err_to_name(err));
  }
#else
  topway_str_write_cached(VP_STR_CAL_STATUS_MSG, "Current zero disabled");
#endif
  topway_n16_write(VP_N16_CAL_ZERO_BTN, 0);
}

static esp_err_t send_calibration_to_topway(void)
{
  if (!s_display_ready) {
    return ESP_ERR_INVALID_STATE;
  }

  esp_err_t result = ESP_OK;
  result = topway_merge_err(result, topway_n16_write_checked(VP_N16_CAL_CURRENT_SCALE,
                                                            current_cal_x100_to_vp(config_get_current_cal_x100())));
  result = topway_merge_err(result, topway_n16_write_checked(VP_N16_CAL_TIN_OFFSET,
                                                            temp_offset_x10_to_vp(config_get_tin_offset_x10())));
  result = topway_merge_err(result, topway_n16_write_checked(VP_N16_CAL_TOUT_OFFSET,
                                                            temp_offset_x10_to_vp(config_get_tout_offset_x10())));
  result = topway_merge_err(result, topway_n16_write_checked(VP_N16_CAL_ZERO_BTN, 0));
  result = topway_merge_err(result, topway_n16_write_checked(VP_N16_CAL_APPLY_BTN, 0));
  result = topway_merge_err(result, topway_n16_write_checked(VP_N16_CAL_SAVE_BTN, 0));
  result = topway_merge_err(result, topway_str_write_cached(VP_STR_CAL_STATUS_MSG, ""));
  return result;
}

static void idms_ui_topway_check_calibration_buttons(void)
{
  if (!s_display_ready) {
    return;
  }
  if (topway_controls_in_backoff()) {
    return;
  }

  uint16_t zero_btn = 0;
  uint16_t apply_btn = 0;
  uint16_t save_btn = 0;
  if (topway_control_n16_read(VP_N16_CAL_ZERO_BTN, &zero_btn) != ESP_OK) return;
  if (topway_control_n16_read(VP_N16_CAL_APPLY_BTN, &apply_btn) != ESP_OK) return;
  if (topway_control_n16_read(VP_N16_CAL_SAVE_BTN, &save_btn) != ESP_OK) return;

  if (zero_btn != 0) {
    zero_current_from_lcd();
  }
  if (apply_btn != 0 || save_btn != 0) {
    apply_lcd_calibration(true);
  }
}

static esp_err_t update_telegram_panel(bool force)
{
  if (!s_display_ready) {
    return ESP_ERR_INVALID_STATE;
  }

  uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
  if (!force && (now - s_last_telegram_panel_update) < UI_TOPWAY_TELEGRAM_POLL_MS) {
    return ESP_OK;
  }
  s_last_telegram_panel_update = now;
  esp_err_t result = ESP_OK;

  char qr_code[CONFIG_INFO_STRING_MAX_LEN + 1] = {0};
  if (config_get_qr_code(qr_code, sizeof(qr_code)) != ESP_OK || qr_code[0] == '\0') {
    copy_info_cache(qr_code, CONFIG_DEFAULT_QR_CODE);
  }
  result = topway_merge_err(result, topway_str_write_cached(VP_STR_TELEGRAM_QR_URL, qr_code));

  const uint32_t rows[CONFIG_TECH_MAX_COUNT] = {
      VP_STR_TELEGRAM_AUTH_ROW0,
      VP_STR_TELEGRAM_AUTH_ROW1,
      VP_STR_TELEGRAM_AUTH_ROW2,
      VP_STR_TELEGRAM_AUTH_ROW3,
      VP_STR_TELEGRAM_AUTH_ROW4,
  };
  uint8_t count = config_get_tech_count();
  for (int i = 0; i < CONFIG_TECH_MAX_COUNT; i++) {
    char row[80] = "";
    if (i < count) {
      char phone[24] = "";
      char id[64] = "";
      config_get_tech_phone(i, phone, sizeof(phone));
      config_get_tech_id(i, id, sizeof(id));
      if (phone[0] != '\0') {
        snprintf(row, sizeof(row), "%d  %s%s", i + 1, phone, id[0] ? "" : "  Pending");
      } else if (id[0] != '\0') {
        snprintf(row, sizeof(row), "%d  Phone required", i + 1);
      }
    }
    result = topway_merge_err(result, topway_str_write_cached(rows[i], row));
  }
  return result;
}

static void idms_ui_topway_check_telegram_authorize_button(void)
{
  if (!s_display_ready) {
    return;
  }
  if (topway_controls_in_backoff()) {
    return;
  }

  uint16_t auth_btn = 0;
  if (topway_control_n16_read(VP_N16_TELEGRAM_AUTHORIZE_BTN, &auth_btn) != ESP_OK) {
    return;
  }
  if (auth_btn == 0) {
    return;
  }

  topway_n16_write(VP_N16_TELEGRAM_AUTHORIZE_BTN, 0);

  char tech_id[64] = {0};
  esp_err_t err = topway_str_read(VP_STR_TELEGRAM_TECH_INPUT, tech_id, sizeof(tech_id));
  if (err != ESP_OK) {
    topway_str_write_cached(VP_STR_TELEGRAM_STATUS_MSG, "Could not read number");
    return;
  }
  trim_ascii_in_place(tech_id);

  char phone[24] = "";
  if (config_normalize_egypt_phone(tech_id, phone, sizeof(phone)) != ESP_OK) {
    topway_str_write_cached(VP_STR_TELEGRAM_STATUS_MSG, "Enter local phone number");
    return;
  }

  int existing_idx = -1;
  if (config_find_tech_phone(phone, &existing_idx) == ESP_OK) {
    topway_str_write_cached(VP_STR_TELEGRAM_STATUS_MSG, "Phone already listed");
    topway_str_write(VP_STR_TELEGRAM_TECH_INPUT, "");
    update_telegram_panel(true);
    return;
  }

  if (config_get_tech_count() >= CONFIG_TECH_MAX_COUNT) {
    topway_str_write_cached(VP_STR_TELEGRAM_STATUS_MSG, "Technician list full");
    return;
  }

  err = config_add_pending_tech_phone(phone, "LCD");
  if (err == ESP_OK) {
    topway_str_write_cached(VP_STR_TELEGRAM_STATUS_MSG, "Phone added. Ask tech to share contact.");
    topway_str_write(VP_STR_TELEGRAM_TECH_INPUT, "");
    update_telegram_panel(true);
  } else {
    ESP_LOGW(TAG, "LCD phone authorization failed: %s", esp_err_to_name(err));
    topway_str_write_cached(VP_STR_TELEGRAM_STATUS_MSG, "Authorization failed");
  }
}

static void idms_ui_topway_check_telegram_delete_buttons(void)
{
  if (!s_display_ready) {
    return;
  }
  if (topway_controls_in_backoff()) {
    return;
  }

  const uint32_t delete_vps[CONFIG_TECH_MAX_COUNT] = {
      VP_N16_TELEGRAM_DELETE_ROW0,
      VP_N16_TELEGRAM_DELETE_ROW1,
      VP_N16_TELEGRAM_DELETE_ROW2,
      VP_N16_TELEGRAM_DELETE_ROW3,
      VP_N16_TELEGRAM_DELETE_ROW4,
  };
  uint8_t count = config_get_tech_count();
  for (int i = 0; i < CONFIG_TECH_MAX_COUNT; i++) {
    uint16_t btn = 0;
    if (topway_control_n16_read(delete_vps[i], &btn) != ESP_OK) {
      return;
    }
    if (btn == 0) {
      continue;
    }

    topway_n16_write(delete_vps[i], 0);
    if (i >= count) {
      topway_str_write_cached(VP_STR_TELEGRAM_STATUS_MSG, "No number in this row");
      continue;
    }

    esp_err_t err = config_remove_tech(i);
    if (err == ESP_OK) {
      topway_str_write_cached(VP_STR_TELEGRAM_STATUS_MSG, "Number deleted");
      update_telegram_panel(true);
    } else {
      ESP_LOGW(TAG, "LCD technician delete failed: %s", esp_err_to_name(err));
      topway_str_write_cached(VP_STR_TELEGRAM_STATUS_MSG, "Delete failed");
    }
    return;
  }
}

void idms_ui_topway_check_apply_button(void) {
  if (!s_display_ready)
    return;
  if (topway_controls_in_backoff())
    return;

  uint16_t apply_btn = 0;
  esp_err_t err = topway_control_n16_read(VP_N16_CFG_APPLY_BTN, &apply_btn);
  if (err != ESP_OK) {
    return;
  }

  if (apply_btn != 0 && s_last_apply_btn == 0) {
    apply_lcd_config(true);
  }

  s_last_apply_btn = apply_btn;
}

void idms_ui_topway_process_touch_event(uint32_t vp, uint16_t value) {
  (void)vp;
  (void)value;
  idms_ui_topway_update();
}

bool idms_ui_topway_get_config(int16_t *min_tin, int16_t *min_tout, uint16_t *min_current,
                                int16_t *max_tin, int16_t *max_tout, uint16_t *max_current) {
  if (!s_cfg_applied) {
    return false;  /* Config not yet applied via LCD */
  }

  if (min_tin) *min_tin = s_cfg_min_tin;
  if (min_tout) *min_tout = s_cfg_min_tout;
  if (min_current) *min_current = s_cfg_min_current;
  if (max_tin) *max_tin = s_cfg_max_tin;
  if (max_tout) *max_tout = s_cfg_max_tout;
  if (max_current) *max_current = s_cfg_max_current;

  return true;
}

esp_err_t idms_ui_topway_read_config(int16_t *min_tin, int16_t *min_tout, uint16_t *min_current,
                                      int16_t *max_tin, int16_t *max_tout, uint16_t *max_current,
                                      int16_t *dt_alert) {
  if (!s_display_ready)
    return ESP_ERR_INVALID_STATE;

  uint16_t val;
  esp_err_t err = ESP_OK;

  if (min_tin) {
    err = topway_n16_read(VP_N16_CFG_MIN_TIN, &val);
    if (err != ESP_OK) return err;
    *min_tin = (int16_t)val;
  }
  if (min_tout) {
    err = topway_n16_read(VP_N16_CFG_MIN_TOUT, &val);
    if (err != ESP_OK) return err;
    *min_tout = (int16_t)val;
  }
  if (min_current) {
    err = topway_n16_read(VP_N16_CFG_MIN_CURRENT, &val);
    if (err != ESP_OK) return err;
    *min_current = val;
  }
  if (max_tin) {
    err = topway_n16_read(VP_N16_CFG_MAX_TIN, &val);
    if (err != ESP_OK) return err;
    *max_tin = (int16_t)val;
  }
  if (max_tout) {
    err = topway_n16_read(VP_N16_CFG_MAX_TOUT, &val);
    if (err != ESP_OK) return err;
    *max_tout = (int16_t)val;
  }
  if (max_current) {
    err = topway_n16_read(VP_N16_CFG_MAX_CURRENT, &val);
    if (err != ESP_OK) return err;
    *max_current = val;
  }
  if (dt_alert) {
    err = topway_n16_read(VP_N16_CFG_DT_ALERT, &val);
    if (err != ESP_OK) return err;
    *dt_alert = (int16_t)val;
  }

  return ESP_OK;
}

static esp_err_t topway_push_full_state(bool clear_wifi_fields, bool reset_controls)
{
  topway_invalidate_write_cache();

  esp_err_t result = ESP_OK;
  result = topway_merge_err(result, update_display_values());
  result = topway_merge_err(result, update_wifi_status_on_lcd(true));
  result = topway_merge_err(result, update_rtc_on_lcd(true));
  result = topway_merge_err(result, send_device_info_to_topway(reset_controls));
  result = topway_merge_err(result, send_config_to_topway());
  result = topway_merge_err(result, send_calibration_to_topway());

  if (clear_wifi_fields) {
    ESP_LOGI(TAG, "Clearing WiFi credential fields on Topway");
    vTaskDelay(pdMS_TO_TICKS(100));
    result = topway_merge_err(result, topway_str_write_checked(VP_STR_WIFI_SSID, ""));
    vTaskDelay(pdMS_TO_TICKS(50));
    result = topway_merge_err(result, topway_str_write_checked(VP_STR_WIFI_PASSWORD, ""));
    if (result == ESP_OK) {
      s_wifi_fields_cleared = true;
    }
  }

  return result;
}

static esp_err_t topway_connect_sequence(void)
{
  ESP_LOGI(TAG, "Trying Topway handshake/setup...");
  esp_err_t err = topway_handshake();
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Topway handshake failed: %s; display not ready yet",
             esp_err_to_name(err));
    s_display_ready = false;
    topway_invalidate_write_cache();
    return err;
  }

  vTaskDelay(pdMS_TO_TICKS(UI_TOPWAY_BOOT_SETTLE_MS));

  err = topway_set_sys_config(topway_baud_code_from_rate(CONFIG_IDMS_TOPWAY_BAUD),
                              TOPWAY_TOUCH_KEY_ID_CFG);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "set_sys_config failed: %s", esp_err_to_name(err));
    s_display_ready = false;
    topway_invalidate_write_cache();
    return err;
  }

  err = topway_set_codepage(1, 12);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "set_codepage failed: %s", esp_err_to_name(err));
    s_display_ready = false;
    topway_invalidate_write_cache();
    return err;
  }

  err = topway_disp_page(PAGE_MAIN);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "disp_page failed: %s", esp_err_to_name(err));
    s_display_ready = false;
    topway_invalidate_write_cache();
    return err;
  }

  topway_register_touch_callback(idms_ui_topway_process_touch_event);

  s_display_ready = true;
  s_topway_io_failures = 0;
  s_last_healthcheck = xTaskGetTickCount() * portTICK_PERIOD_MS;
  s_last_periodic_resync = s_last_healthcheck;
  s_wifi_fields_cleared = false;

  ESP_LOGI(TAG, "Topway ready; sending full UI state");
  err = topway_push_full_state(true, true);
  if (err != ESP_OK) {
    topway_mark_not_ready("full-state sync", err);
    return err;
  }
  s_full_resync_due = xTaskGetTickCount() * portTICK_PERIOD_MS + UI_TOPWAY_POST_CONNECT_RESYNC_MS;
  s_full_resync_pending = true;

  ESP_LOGI(TAG, "Topway handshake/setup complete");
  return ESP_OK;
}

static void topway_healthcheck(void)
{
  uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
  if ((now - s_last_healthcheck) < UI_TOPWAY_HEALTHCHECK_MS) {
    return;
  }
  s_last_healthcheck = now;

  esp_err_t err = topway_handshake();
  if (err != ESP_OK) {
    topway_mark_not_ready("healthcheck", err);
  }
}

static void topway_maybe_resync(void)
{
  uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;

  if (s_full_resync_pending && now >= s_full_resync_due) {
    s_full_resync_pending = false;
    s_last_periodic_resync = now;
    ESP_LOGI(TAG, "Sending delayed Topway full-state refresh");
    esp_err_t err = topway_push_full_state(false, true);
    if (err != ESP_OK) {
      topway_mark_not_ready("full-state refresh", err);
    }
    return;
  }

  if ((now - s_last_periodic_resync) < UI_TOPWAY_PERIODIC_RESYNC_MS) {
    return;
  }

  s_last_periodic_resync = now;
  ESP_LOGI(TAG, "Sending periodic Topway full-state refresh");
  esp_err_t err = topway_push_full_state(false, false);
  if (err != ESP_OK) {
    topway_mark_not_ready("periodic full-state refresh", err);
  }
}

static void ui_topway_task(void *arg) {
  (void)arg;

  for (;;) {
    if (!s_display_ready) {
      uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
      if ((now - s_last_reconnect_attempt) >= UI_TOPWAY_RECONNECT_MS) {
        s_last_reconnect_attempt = now;
        topway_connect_sequence();
      }
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(UI_TOPWAY_REFRESH_MS));
      continue;
    }

    topway_healthcheck();
    if (!s_display_ready) {
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(UI_TOPWAY_REFRESH_MS));
      continue;
    }

    topway_maybe_resync();
    if (!s_display_ready) {
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(UI_TOPWAY_REFRESH_MS));
      continue;
    }

    update_display_values();
    (void)update_wifi_status_on_lcd(false);
    update_rtc_on_lcd(false);
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if (now >= s_control_backoff_until &&
        (now - s_last_control_poll) >= UI_TOPWAY_CONTROL_POLL_MS) {
      s_last_control_poll = now;
      idms_ui_topway_check_apply_button();
      idms_ui_topway_check_wifi_button();
      idms_ui_topway_check_calibration_buttons();
      idms_ui_topway_check_telegram_authorize_button();
      idms_ui_topway_check_telegram_delete_buttons();
    }
    update_telegram_panel(false);
    topway_process_touch_events();
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(UI_TOPWAY_REFRESH_MS));
  }
}

esp_err_t idms_ui_topway_init(void) {
  ESP_LOGI(TAG, "Initializing Topway UI (HKT070DTA-1C, 800x480)");

  setenv("TZ", TZ_AFRICA_CAIRO, 1);
  tzset();

  topway_config_t config = {
      .uart_port = UART_NUM_1,
      .tx_pin = CONFIG_IDMS_PIN_UART_TX,
      .rx_pin = CONFIG_IDMS_PIN_UART_RX,
      .rts_pin = CONFIG_IDMS_PIN_UART_RTS,
        .baud_rate = CONFIG_IDMS_TOPWAY_BAUD,
  };

  esp_err_t err = topway_init(&config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Topway init failed: %s", esp_err_to_name(err));
    return err;
  }

  vTaskDelay(pdMS_TO_TICKS(200));

  err = topway_connect_sequence();
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Topway not connected yet; UI task will keep retrying");
  }

  if (!s_ui_task) {
    BaseType_t task_ok = xTaskCreatePinnedToCore(ui_topway_task, "ui_topway",
                                                 8192, NULL, 4, &s_ui_task,
                                                 tskNO_AFFINITY);
    if (task_ok != pdPASS) {
      s_ui_task = NULL;
      ESP_LOGE(TAG, "Failed to start Topway UI task");
      return ESP_ERR_NO_MEM;
    }
  }

  ESP_LOGI(TAG, "Topway UI task ready (800x480, full refresh every %d ms)",
           CONFIG_IDMS_TOPWAY_FULL_REFRESH_MS);
  return ESP_OK;
}

void idms_ui_topway_update(void) {
  if (s_ui_task) {
    xTaskNotifyGive(s_ui_task);
  }
}

static esp_err_t send_device_info_to_topway(bool reset_controls) {
  if (!s_display_ready)
    return ESP_ERR_INVALID_STATE;

  ESP_LOGI(TAG, "Sending Telegram page values to Topway LCD");
  esp_err_t result = ESP_OK;
  if (reset_controls) {
    result = topway_merge_err(result, topway_str_write_checked(VP_STR_TELEGRAM_TECH_INPUT, ""));
    result = topway_merge_err(result, topway_str_write_cached(VP_STR_TELEGRAM_STATUS_MSG, ""));
    result = topway_merge_err(result, topway_n16_write_checked(VP_N16_TELEGRAM_AUTHORIZE_BTN, 0));
    result = topway_merge_err(result, topway_n16_write_checked(VP_N16_TELEGRAM_DELETE_ROW0, 0));
    result = topway_merge_err(result, topway_n16_write_checked(VP_N16_TELEGRAM_DELETE_ROW1, 0));
    result = topway_merge_err(result, topway_n16_write_checked(VP_N16_TELEGRAM_DELETE_ROW2, 0));
    result = topway_merge_err(result, topway_n16_write_checked(VP_N16_TELEGRAM_DELETE_ROW3, 0));
    result = topway_merge_err(result, topway_n16_write_checked(VP_N16_TELEGRAM_DELETE_ROW4, 0));
  }
  result = topway_merge_err(result, update_telegram_panel(true));
  return result;
}

void idms_ui_topway_send_device_info(void) {
  (void)send_device_info_to_topway(true);
}

static esp_err_t send_config_to_topway(void) {
  if (!s_display_ready)
    return ESP_ERR_INVALID_STATE;

  ESP_LOGI(TAG, "Sending config parameters to Topway LCD");

  /* Read from NVS and send to LCD */
  int16_t min_tin = config_get_min_tin();
  int16_t max_tin = config_get_max_tin();
  int16_t min_tout = config_get_min_tout();
  int16_t max_tout = config_get_max_tout();
  uint16_t min_current = config_get_min_current();
  uint16_t max_current = config_get_max_current();
  int16_t dt_alert = config_get_dt_alert_threshold();

  s_cfg_min_tin = min_tin;
  s_cfg_max_tin = max_tin;
  s_cfg_min_tout = min_tout;
  s_cfg_max_tout = max_tout;
  s_cfg_min_current = min_current;
  s_cfg_max_current = max_current;
  s_cfg_dt_alert = dt_alert;

  esp_err_t result = ESP_OK;

  /* Send Min Temp IN threshold (pure Celsius) */
  result = topway_merge_err(result, topway_n16_write_checked(VP_N16_CFG_MIN_TIN, (uint16_t)min_tin));
  ESP_LOGI(TAG, "Min Temp IN: %d C", min_tin);

  /* Send Max Temp IN threshold (pure Celsius) */
  result = topway_merge_err(result, topway_n16_write_checked(VP_N16_CFG_MAX_TIN, (uint16_t)max_tin));
  ESP_LOGI(TAG, "Max Temp IN: %d C", max_tin);

  /* Send Min Temp OUT threshold (pure Celsius) */
  result = topway_merge_err(result, topway_n16_write_checked(VP_N16_CFG_MIN_TOUT, (uint16_t)min_tout));
  ESP_LOGI(TAG, "Min Temp OUT: %d C", min_tout);

  /* Send Max Temp OUT threshold (pure Celsius) */
  result = topway_merge_err(result, topway_n16_write_checked(VP_N16_CFG_MAX_TOUT, (uint16_t)max_tout));
  ESP_LOGI(TAG, "Max Temp OUT: %d C", max_tout);

  /* Send Min Current threshold (pure Amperes) */
  result = topway_merge_err(result, topway_n16_write_checked(VP_N16_CFG_MIN_CURRENT, min_current));
  ESP_LOGI(TAG, "Min Current: %d A", min_current);

  /* Send Max Current threshold (pure Amperes) */
  result = topway_merge_err(result, topway_n16_write_checked(VP_N16_CFG_MAX_CURRENT, max_current));
  ESP_LOGI(TAG, "Max Current: %d A", max_current);

  /* Send Delta_T threshold (pure Celsius) */
  result = topway_merge_err(result, topway_n16_write_checked(VP_N16_CFG_DT_ALERT, (uint16_t)dt_alert));
  ESP_LOGI(TAG, "Delta_T threshold: %d C", dt_alert);

  result = topway_merge_err(result, topway_n16_write_checked(VP_N16_CFG_APPLY_BTN, 0));
  result = topway_merge_err(result, topway_str_write_cached(VP_STR_CFG_STATUS_MSG, ""));
  return result;
}

void idms_ui_topway_send_config(void) {
  (void)send_config_to_topway();
}

void idms_ui_topway_check_wifi_button(void)
{
  if (!s_display_ready)
    return;
  if (topway_controls_in_backoff())
    return;

  uint16_t wifi_btn = 0;
  
  esp_err_t err = topway_control_n16_read(VP_N16_WIFI_CONNECT_BTN, &wifi_btn);
  if (err != ESP_OK) {
    return;
  }

  uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;

  /* Clear button if it's stuck at non-zero (LCD toggle button behavior) */
  if (wifi_btn != 0) {
    topway_n16_write(VP_N16_WIFI_CONNECT_BTN, 0);
  }

  /* Check for button press with cooldown (handles both edge and toggle modes) */
  if (wifi_btn != 0 && (now - s_wifi_btn_last_trigger) > WIFI_BTN_COOLDOWN_MS) {
    char ssid[64] = {0};
    char password[64] = {0};

    ESP_LOGI(TAG, "WiFi Connect button pressed (btn=%d)", wifi_btn);
    s_wifi_btn_last_trigger = now;  /* Record trigger time for cooldown */

    /* Read WiFi SSID from LCD */
    esp_err_t ssid_err = topway_str_read(VP_STR_WIFI_SSID, ssid, sizeof(ssid));
    if (ssid_err != ESP_OK) {
      ESP_LOGW(TAG, "Failed to read WiFi SSID from LCD: %s", esp_err_to_name(ssid_err));
      return;
    }

    /* Read WiFi Password from LCD */
    esp_err_t pass_err = topway_str_read(VP_STR_WIFI_PASSWORD, password, sizeof(password));
    if (pass_err != ESP_OK) {
      ESP_LOGW(TAG, "Failed to read WiFi password from LCD: %s", esp_err_to_name(pass_err));
      return;
    }

    ESP_LOGI(TAG, "WiFi credentials read: SSID='%s' (len=%d), Pass='%s' (len=%d)",
             ssid, strlen(ssid), strlen(password) > 0 ? "***" : "(empty)", strlen(password));

    /* Validate - SSID cannot be empty or placeholder */
    if (ssid[0] == '\0' || strcmp(ssid, "--") == 0 || strlen(ssid) < 2) {
      /* Only clear fields and show warning if WiFi is not connected */
      if (!network_manager_is_connected()) {
        ESP_LOGW(TAG, "WiFi SSID invalid or placeholder: '%s' - clearing fields", ssid);
        topway_str_write_cached(VP_STR_WIFI_STATUS_MSG, "WiFi: Enter SSID/Pass");
        topway_str_write(VP_STR_WIFI_SSID, "");
        topway_str_write(VP_STR_WIFI_PASSWORD, "");
      } else {
        ESP_LOGI(TAG, "WiFi already connected, ignoring empty button press");
      }
      return;
    }

    /* Validate - Password max length 63 chars (WPA2 limit) */
    if (strlen(password) > 63) {
      ESP_LOGW(TAG, "WiFi password too long (>63 chars)");
      return;
    }

    ESP_LOGI(TAG, "WiFi config received: SSID='%s'", ssid);

    /* Get current credentials for comparison */
    char current_ssid[64] = {0};
    char current_pass[64] = {0};
    config_get_wifi_ssid(current_ssid, sizeof(current_ssid));
    config_get_wifi_password(current_pass, sizeof(current_pass));
    
    /* If password is empty, use existing password (user only changed SSID) */
    if (password[0] == '\0' && current_pass[0] != '\0') {
      strncpy(password, current_pass, sizeof(password) - 1);
      password[sizeof(password) - 1] = '\0';
      ESP_LOGI(TAG, "Using existing password for new SSID");
    }

    /* Check if already connected with same credentials - skip if so */
    if (network_manager_is_connected() && 
        strcmp(current_ssid, ssid) == 0 && 
        strcmp(current_pass, password) == 0) {
      ESP_LOGI(TAG, "WiFi already connected with same credentials, skipping reconnect");
      topway_str_write(VP_STR_WIFI_PASSWORD, "");  /* Clear password field */
      return;
    }
    
    /* If connected with same SSID but different password, save but don't reconnect */
    if (network_manager_is_connected() && strcmp(current_ssid, ssid) == 0) {
      ESP_LOGI(TAG, "WiFi connected - saving new password for next connection");
      if (config_set_wifi_password(password) == ESP_OK) {
        topway_str_write(VP_STR_WIFI_PASSWORD, "");
        topway_str_write_cached(VP_STR_WIFI_STATUS_MSG, "WiFi: Pass saved");
        ESP_LOGI(TAG, "New password saved. Will use on next connect.");
      }
      return;
    }
    
    /* Validate password is not empty for secure networks */
    if (password[0] == '\0') {
      ESP_LOGW(TAG, "WiFi password is empty - cannot connect to secure network");
      topway_str_write_cached(VP_STR_WIFI_STATUS_MSG, "WiFi: Enter Password");
      return;
    }

    /* Changing to a new network - save and reconnect */
    ESP_LOGI(TAG, "Changing WiFi network: '%s' -> '%s'", current_ssid, ssid);
    
    /* Save to NVS */
    if (config_set_wifi_ssid(ssid) == ESP_OK &&
        config_set_wifi_password(password) == ESP_OK) {
      ESP_LOGI(TAG, "WiFi credentials saved to NVS, reconnecting to new network...");
      
      /* Clear password field for security */
      topway_str_write(VP_STR_WIFI_PASSWORD, "");
      
      /* Show connecting status */
      topway_str_write_cached(VP_STR_WIFI_STATUS_MSG, "WiFi: Changing Network...");
      
      /* Trigger WiFi reconnect */
      network_manager_reconnect();
    } else {
      ESP_LOGE(TAG, "Failed to save WiFi credentials to NVS");
      topway_str_write_cached(VP_STR_WIFI_STATUS_MSG, "WiFi: Save Error");
    }
  }
}
