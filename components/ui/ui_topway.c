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
#include "ota.h"
#include "wifi_manager.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "esp_chip_info.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "ui_topway";

#define PAGE_MAIN 0
#define INFO_FIELD_MAX_LEN (CONFIG_INFO_STRING_MAX_LEN + 1)
#define INFO_FIELD_POLL_MS 1500

static TaskHandle_t s_ui_task = NULL;
static bool s_display_ready = false;
static float s_last_current = -999.0f;
static float s_last_tin = -999.0f;
static float s_last_tout = -999.0f;
static float s_last_dt = -999.0f;
static const char *s_last_diag = "";
static uint16_t s_last_apply_btn = 0;  /* Track apply button state */

/* Runtime config values - can be modified via LCD */
static int16_t  s_cfg_min_tin = 0;
static int16_t  s_cfg_min_tout = 0;
static uint16_t s_cfg_min_current = 0;
static int16_t  s_cfg_max_tin = 0;
static int16_t  s_cfg_max_tout = 0;
static uint16_t s_cfg_max_current = 0;
static int16_t  s_cfg_dt_alert = 0;
static bool     s_cfg_applied = false;

static char s_info_device_model[INFO_FIELD_MAX_LEN] = "";
static char s_info_serial_number[INFO_FIELD_MAX_LEN] = "";
static char s_info_manufacture_date[INFO_FIELD_MAX_LEN] = "";
static char s_info_support_email[INFO_FIELD_MAX_LEN] = "";
static char s_info_support_phone[INFO_FIELD_MAX_LEN] = "";
static char s_info_qr_code[INFO_FIELD_MAX_LEN] = "";
static uint32_t s_info_last_poll = 0;
static size_t s_info_next_field = 0;

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

static void generate_default_serial_number(char *out, size_t out_sz)
{
  if (!out || out_sz == 0) {
    return;
  }
  uint8_t mac[6];
  esp_efuse_mac_get_default(mac);
  snprintf(out, out_sz, "%02X%02X%02X%02X%02X%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void generate_default_manufacture_date(char *out, size_t out_sz)
{
  if (!out || out_sz == 0) {
    return;
  }
  strncpy(out, "Unknown", out_sz - 1);
  out[out_sz - 1] = '\0';
  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_app_desc_t app_desc;
  if (running && esp_ota_get_partition_description(running, &app_desc) == ESP_OK) {
    snprintf(out, out_sz, "%s", app_desc.date);
  }
}

typedef esp_err_t (*info_setter_t)(const char *value);

typedef struct {
  uint32_t vp;
  char *cache;
  const char *label;
  info_setter_t set;
} info_field_t;

static void idms_ui_topway_poll_info_fields(void)
{
  if (!s_display_ready) {
    return;
  }

  uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
  if ((now - s_info_last_poll) < INFO_FIELD_POLL_MS) {
    return;
  }
  s_info_last_poll = now;

  static info_field_t fields[] = {
      { VP_STR_DEVICE_MODEL, s_info_device_model, "Device Model", config_set_device_model },
      { VP_STR_SERIAL_NUMBER, s_info_serial_number, "Serial Number", config_set_serial_number },
      { VP_STR_MANUFACTURE_DATE, s_info_manufacture_date, "Manufacture Date", config_set_manufacture_date },
      { VP_STR_SUPPORT_EMAIL, s_info_support_email, "Support Email", config_set_support_email },
      { VP_STR_SUPPORT_PHONE, s_info_support_phone, "Support Phone", config_set_support_phone },
      { VP_STR_QR_CODE, s_info_qr_code, "QR Code", config_set_qr_code },
  };
  const size_t field_count = sizeof(fields) / sizeof(fields[0]);
  info_field_t *field = &fields[s_info_next_field];
  s_info_next_field = (s_info_next_field + 1) % field_count;

  char value[INFO_FIELD_MAX_LEN] = {0};
  esp_err_t err = topway_str_read(field->vp, value, sizeof(value));
  if (err != ESP_OK) {
    return;
  }

  if (strcmp(value, field->cache) == 0) {
    return;
  }

  err = field->set(value);
  if (err == ESP_OK) {
    copy_info_cache(field->cache, value);
    ESP_LOGI(TAG, "%s updated from LCD and saved to NVS", field->label);
  } else {
    ESP_LOGW(TAG, "Failed to save %s from LCD: %s", field->label, esp_err_to_name(err));
    topway_str_write(field->vp, field->cache);
  }
}

static void update_wifi_status_on_lcd(void)
{
  if (!s_display_ready)
    return;

  uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
  bool is_connected = wifi_manager_is_connected();

  /* Track when we were last connected */
  if (is_connected) {
    s_wifi_last_connected_tick = now;
  }

  /* Only update when state changes or on interval */
  if (is_connected == s_last_wifi_connected &&
      (now - s_wifi_status_last_update) < WIFI_STATUS_UPDATE_INTERVAL_MS) {
    return;
  }

  s_last_wifi_connected = is_connected;
  s_wifi_status_last_update = now;

  if (is_connected) {
    /* Connected - show IP and current SSID */
    char ip[16] = {0};
    char ssid[64] = {0};
    wifi_manager_get_ip(ip, sizeof(ip));
    config_get_wifi_ssid(ssid, sizeof(ssid));

    char status_msg[32];
    snprintf(status_msg, sizeof(status_msg), "WiFi: %s", ip);
    topway_str_write(VP_STR_WIFI_STATUS_MSG, status_msg);

    /* Show connected SSID in the SSID field */
    if (ssid[0] != '\0') {
      topway_str_write(VP_STR_WIFI_SSID, ssid);
    }
  } else {
    /* Disconnected - check if credentials are configured */
    char ssid[64] = {0};
    config_get_wifi_ssid(ssid, sizeof(ssid));

    if (ssid[0] == '\0') {
      topway_str_write(VP_STR_WIFI_STATUS_MSG, "WiFi: Not Configured");
    } else if ((now - s_wifi_last_connected_tick) < WIFI_CONNECTION_TIMEOUT_MS) {
      /* Recently disconnected, might be reconnecting */
      topway_str_write(VP_STR_WIFI_STATUS_MSG, "WiFi: Connecting...");
    } else {
      /* Connection failed or timeout */
      topway_str_write(VP_STR_WIFI_STATUS_MSG, "WiFi: Check Password");
    }
  }
}

static void update_display_values(void) {
  if (!s_display_ready)
    return;

  idms_metrics_t m;
  monitor_get_metrics(&m);

  float current_rounded = m.current_valid ? (float)(int)(m.current_a * 10.0f + 0.5f) / 10.0f : -999.0f;
  float tin_rounded = m.t_in_valid ? (float)(int)(m.t_in_c * 10.0f + 0.5f) / 10.0f : -999.0f;
  float tout_rounded = m.t_out_valid ? (float)(int)(m.t_out_c * 10.0f + 0.5f) / 10.0f : -999.0f;
  float dt_rounded = m.delta_valid ? (float)(int)(m.delta_t_c * 10.0f + 0.5f) / 10.0f : -999.0f;

  if (m.current_valid) {
    uint16_t cur_x10 = (uint16_t)(m.current_a * 10.0f + 0.5f);
    topway_n16_write(VP_N16_CUR_X10, cur_x10);
    topway_n16_write(VP_N16_CUR_VALID, 1);
    if (current_rounded != s_last_current) {
      ESP_LOGI(TAG, "Current=%.1fA", m.current_a);
      s_last_current = current_rounded;
    }
  } else {
    topway_n16_write(VP_N16_CUR_VALID, 0);
    topway_str_write(VP_STR_CURRENT, "--");
    if (s_last_current != -888.0f) {
      ESP_LOGI(TAG, "Current=-- (invalid)");
      s_last_current = -888.0f;
    }
  }

  if (m.t_in_valid) {
    uint16_t tin_x10 = (uint16_t)(m.t_in_c * 10.0f + 0.5f);
    topway_n16_write(VP_N16_TIN_X10, tin_x10);
    topway_n16_write(VP_N16_TIN_VALID, 1);
    if (tin_rounded != s_last_tin) {
      ESP_LOGI(TAG, "T_in=%.1fC", m.t_in_c);
      s_last_tin = tin_rounded;
    }
  } else {
    topway_n16_write(VP_N16_TIN_VALID, 0);
  }

  if (m.t_out_valid) {
    uint16_t tout_x10 = (uint16_t)(m.t_out_c * 10.0f + 0.5f);
    topway_n16_write(VP_N16_TOUT_X10, tout_x10);
    topway_n16_write(VP_N16_TOUT_VALID, 1);
    if (tout_rounded != s_last_tout) {
      ESP_LOGI(TAG, "T_out=%.1fC", m.t_out_c);
      s_last_tout = tout_rounded;
    }
  } else {
    topway_n16_write(VP_N16_TOUT_VALID, 0);
  }

  if (m.delta_valid) {
    int16_t dt_x10 = (int16_t)(m.delta_t_c * 10.0f + 0.5f);
    topway_n16_write(VP_N16_DT_X10, (uint16_t)dt_x10);
    topway_n16_write(VP_N16_DT_VALID, 1);
    if (dt_rounded != s_last_dt) {
      ESP_LOGI(TAG, "dT=%.1fC", m.delta_t_c);
      s_last_dt = dt_rounded;
    }
    
    topway_n16_write(VP_N16_COOL_FAULT, m.cooling_fault ? 1 : 0);
    topway_n16_write(VP_N16_DT_ALERT, m.delta_alert ? 1 : 0);
  } else {
    topway_n16_write(VP_N16_DT_VALID, 0);
    topway_n16_write(VP_N16_COOL_FAULT, 0);
    topway_n16_write(VP_N16_DT_ALERT, 0);
  }

  if (wifi_manager_is_connected()) {
    topway_n16_write(VP_N16_WIFI_STATUS, WIFI_STATUS_CONNECTED);
    topway_str_write(VP_STR_WIFI, m.wifi_ip[0] ? m.wifi_ip : "Connected");
  } else {
    topway_n16_write(VP_N16_WIFI_STATUS, WIFI_STATUS_OFFLINE);
    topway_str_write(VP_STR_WIFI, "Offline");
  }

  const char *ota_status = ota_get_status();
  if (strcmp(ota_status, "ready") == 0) {
    topway_n16_write(VP_N16_OTA_STATUS, OTA_STATUS_READY);
  } else if (strcmp(ota_status, "updating") == 0) {
    topway_n16_write(VP_N16_OTA_STATUS, OTA_STATUS_UPDATING);
  } else {
    topway_n16_write(VP_N16_OTA_STATUS, OTA_STATUS_ERROR);
  }
  topway_str_write(VP_STR_OTA, ota_status);

  topway_n16_write(VP_N16_TECH_COUNT, config_get_tech_count());

  topway_n16_write(VP_N16_POWER_FAULT, m.power_fault ? 1 : 0);

  /* Determine device status and diagnostic message */
  const char *status_str = "ACTIVE";
  const char *diag_str = "OK";

  bool has_power_fault = m.power_fault;
  bool has_cool_fault = m.cooling_fault;
  bool temp_invalid = (!m.t_in_valid || !m.t_out_valid);
  bool current_invalid = !m.current_valid;

  if (m.sensor_preflight_done && !m.sensor_preflight_ok) {
    status_str = "ERROR";
    diag_str = m.sensor_status[0] ? m.sensor_status : "Sensor Preflight";
  } else if (temp_invalid && current_invalid) {
    status_str = "ERROR";
    diag_str = "Sensors Offline";
  } else if (has_power_fault && has_cool_fault) {
    status_str = "ERROR";
    diag_str = "Power + Cooling";
  } else if (has_power_fault) {
    status_str = "ERROR";
    diag_str = "Power Loss";
  } else if (has_cool_fault) {
    status_str = "ERROR";
    diag_str = "Cooling Fault";
  } else if (temp_invalid) {
    status_str = "WARNING";
    diag_str = "Temp Sensor";
  } else if (current_invalid) {
    status_str = "WARNING";
    diag_str = "Current Sensor";
  } else if (m.current_valid && m.current_a < 0.5f) {
    status_str = "INACTIVE";
    diag_str = "Standby";
  }

  /* Status word goes to the color-coded VPs (0x000600=ERROR, 0x000700=WARNING) */
  topway_str_write(VP_STR_STATUS_ERR, (strcmp(status_str, "ERROR") == 0) ? status_str : "");
  topway_str_write(VP_STR_STATUS_WARN, (strcmp(status_str, "WARNING") == 0) ? status_str : "");
  topway_str_write(VP_STR_STATUS, (strcmp(status_str, "ACTIVE") == 0 || strcmp(status_str, "INACTIVE") == 0) ? status_str : "");
  /* Diagnostic detail goes to its own VP */
  topway_str_write(VP_STR_DIAG, diag_str);
  if (strcmp(diag_str, s_last_diag) != 0) {
    ESP_LOGI(TAG, "Status=%s Diag=%s", status_str, diag_str);
    s_last_diag = diag_str;
  }
}

void idms_ui_topway_check_apply_button(void) {
  if (!s_display_ready)
    return;

  uint16_t apply_btn = 0;
  esp_err_t err = topway_n16_read(VP_N16_CFG_APPLY_BTN, &apply_btn);
  
  if (err != ESP_OK) {
    return;
  }

  /* Check for rising edge (0 -> 1) */
  if (apply_btn != 0 && s_last_apply_btn == 0) {
    /* Read all config values from LCD */
    uint16_t val;
    int16_t new_min_tin = s_cfg_min_tin;
    int16_t new_max_tin = s_cfg_max_tin;
    int16_t new_min_tout = s_cfg_min_tout;
    int16_t new_max_tout = s_cfg_max_tout;
    uint16_t new_min_current = s_cfg_min_current;
    uint16_t new_max_current = s_cfg_max_current;
    int16_t new_dt_alert = s_cfg_dt_alert;
    bool valid = true;
    
    /* Read and validate Min Temp IN: -50 to 100 C */
    if (topway_n16_read(VP_N16_CFG_MIN_TIN, &val) == ESP_OK) {
      new_min_tin = (int16_t)val;
      if (new_min_tin < CONFIG_TEMP_MIN_LIMIT || new_min_tin > CONFIG_TEMP_MAX_LIMIT) valid = false;
    }
    
    /* Read and validate Max Temp IN: -50 to 100 C */
    if (topway_n16_read(VP_N16_CFG_MAX_TIN, &val) == ESP_OK) {
      new_max_tin = (int16_t)val;
      if (new_max_tin < CONFIG_TEMP_MIN_LIMIT || new_max_tin > CONFIG_TEMP_MAX_LIMIT) valid = false;
    }
    
    /* Read and validate Min Temp OUT: -50 to 100 C */
    if (topway_n16_read(VP_N16_CFG_MIN_TOUT, &val) == ESP_OK) {
      new_min_tout = (int16_t)val;
      if (new_min_tout < CONFIG_TEMP_MIN_LIMIT || new_min_tout > CONFIG_TEMP_MAX_LIMIT) valid = false;
    }
    
    /* Read and validate Max Temp OUT: -50 to 100 C */
    if (topway_n16_read(VP_N16_CFG_MAX_TOUT, &val) == ESP_OK) {
      new_max_tout = (int16_t)val;
      if (new_max_tout < CONFIG_TEMP_MIN_LIMIT || new_max_tout > CONFIG_TEMP_MAX_LIMIT) valid = false;
    }
    
    /* Read and validate Min Current: 0 to 30 A */
    if (topway_n16_read(VP_N16_CFG_MIN_CURRENT, &val) == ESP_OK) {
      new_min_current = val;
      if (new_min_current > CONFIG_CURRENT_MAX_LIMIT) valid = false;
    }
    
    /* Read and validate Max Current: 0 to 30 A */
    if (topway_n16_read(VP_N16_CFG_MAX_CURRENT, &val) == ESP_OK) {
      new_max_current = val;
      if (new_max_current > CONFIG_CURRENT_MAX_LIMIT) valid = false;
    }

    /* Read and validate Delta_T threshold: 0 to 100 C */
    if (topway_n16_read(VP_N16_CFG_DT_ALERT, &val) == ESP_OK) {
      new_dt_alert = (int16_t)val;
      if (new_dt_alert < 0 || new_dt_alert > 100) valid = false;
    }
    
    if (!valid) {
      topway_n16_write(VP_N16_CFG_APPLY_BTN, 0);
      s_last_apply_btn = apply_btn;
      return;
    }
    
    /* Save to NVS */
    esp_err_t save_err = ESP_OK;
    
    if (config_set_min_tin(new_min_tin) != ESP_OK) save_err = ESP_FAIL;
    if (config_set_max_tin(new_max_tin) != ESP_OK) save_err = ESP_FAIL;
    if (config_set_min_tout(new_min_tout) != ESP_OK) save_err = ESP_FAIL;
    if (config_set_max_tout(new_max_tout) != ESP_OK) save_err = ESP_FAIL;
    if (config_set_min_current(new_min_current) != ESP_OK) save_err = ESP_FAIL;
    if (config_set_max_current(new_max_current) != ESP_OK) save_err = ESP_FAIL;
    if (config_set_dt_alert_threshold(new_dt_alert) != ESP_OK) save_err = ESP_FAIL;
    
    if (save_err == ESP_OK) {
      /* Update runtime values */
      s_cfg_min_tin = new_min_tin;
      s_cfg_max_tin = new_max_tin;
      s_cfg_min_tout = new_min_tout;
      s_cfg_max_tout = new_max_tout;
      s_cfg_min_current = new_min_current;
      s_cfg_max_current = new_max_current;
      s_cfg_dt_alert = new_dt_alert;
      s_cfg_applied = true;
      
      ESP_LOGI(TAG, "Config saved to NVS: MinTin=%d, MaxTin=%d, MinTout=%d, MaxTout=%d, MinCur=%dA, MaxCur=%dA, DeltaT=%dC",
               s_cfg_min_tin, s_cfg_max_tin,
               s_cfg_min_tout, s_cfg_max_tout,
               s_cfg_min_current, s_cfg_max_current, s_cfg_dt_alert);
    } else {
      ESP_LOGE(TAG, "Failed to save config to NVS");
    }
    
    /* Clear the button state on LCD to acknowledge */
    topway_n16_write(VP_N16_CFG_APPLY_BTN, 0);
  }
  
  s_last_apply_btn = apply_btn;
}

void idms_ui_topway_process_touch_event(uint8_t page_id, uint8_t key_id) {
  /* Apply button on config page - key_id should match your LCD configuration */
  if (key_id == 1) {
    uint16_t val;
    int16_t new_min_tin = 0, new_max_tin = 0, new_min_tout = 0, new_max_tout = 0;
    uint16_t new_min_current = 0, new_max_current = 0;
    int16_t new_dt_alert = 0;
    bool valid = true;
    
    /* Read all config values from LCD */
    if (topway_n16_read(VP_N16_CFG_MIN_TIN, &val) == ESP_OK) new_min_tin = (int16_t)val;
    if (topway_n16_read(VP_N16_CFG_MAX_TIN, &val) == ESP_OK) new_max_tin = (int16_t)val;
    if (topway_n16_read(VP_N16_CFG_MIN_TOUT, &val) == ESP_OK) new_min_tout = (int16_t)val;
    if (topway_n16_read(VP_N16_CFG_MAX_TOUT, &val) == ESP_OK) new_max_tout = (int16_t)val;
    if (topway_n16_read(VP_N16_CFG_MIN_CURRENT, &val) == ESP_OK) new_min_current = val;
    if (topway_n16_read(VP_N16_CFG_MAX_CURRENT, &val) == ESP_OK) new_max_current = val;
    if (topway_n16_read(VP_N16_CFG_DT_ALERT, &val) == ESP_OK) new_dt_alert = (int16_t)val;
    
    /* Validate */
    if (new_min_tin < CONFIG_TEMP_MIN_LIMIT || new_min_tin > CONFIG_TEMP_MAX_LIMIT) valid = false;
    if (new_max_tin < CONFIG_TEMP_MIN_LIMIT || new_max_tin > CONFIG_TEMP_MAX_LIMIT) valid = false;
    if (new_min_tout < CONFIG_TEMP_MIN_LIMIT || new_min_tout > CONFIG_TEMP_MAX_LIMIT) valid = false;
    if (new_max_tout < CONFIG_TEMP_MIN_LIMIT || new_max_tout > CONFIG_TEMP_MAX_LIMIT) valid = false;
    if (new_min_current > CONFIG_CURRENT_MAX_LIMIT) valid = false;
    if (new_max_current > CONFIG_CURRENT_MAX_LIMIT) valid = false;
    if (new_dt_alert < 0 || new_dt_alert > 100) valid = false;
    
    if (!valid) {
      ESP_LOGW(TAG, "Config validation failed");
      return;
    }
    
    /* Save to NVS */
    if (config_set_min_tin(new_min_tin) == ESP_OK &&
        config_set_max_tin(new_max_tin) == ESP_OK &&
        config_set_min_tout(new_min_tout) == ESP_OK &&
        config_set_max_tout(new_max_tout) == ESP_OK &&
        config_set_min_current(new_min_current) == ESP_OK &&
        config_set_max_current(new_max_current) == ESP_OK &&
        config_set_dt_alert_threshold(new_dt_alert) == ESP_OK) {
      
      s_cfg_min_tin = new_min_tin;
      s_cfg_max_tin = new_max_tin;
      s_cfg_min_tout = new_min_tout;
      s_cfg_max_tout = new_max_tout;
      s_cfg_min_current = new_min_current;
      s_cfg_max_current = new_max_current;
      s_cfg_dt_alert = new_dt_alert;
      s_cfg_applied = true;
      
      ESP_LOGI(TAG, "Config updated: Tin(%d,%d) Tout(%d,%d) Cur(%d,%d) DeltaT=%d",
               new_min_tin, new_max_tin, new_min_tout, new_max_tout, 
               new_min_current, new_max_current, new_dt_alert);
    }
  }
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

static void ui_topway_task(void *arg) {
  (void)arg;

  for (;;) {
    update_display_values();
    update_wifi_status_on_lcd();
    idms_ui_topway_check_apply_button();
    idms_ui_topway_check_wifi_button();
    idms_ui_topway_poll_info_fields();
    topway_process_touch_events();
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(UI_TOPWAY_REFRESH_MS));
  }
}

esp_err_t idms_ui_topway_init(void) {
  ESP_LOGI(TAG, "Initializing Topway UI (HKT070DTA-1C, 800x480)");

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

  err = topway_set_sys_config(topway_baud_code_from_rate(CONFIG_IDMS_TOPWAY_BAUD),
                              TOPWAY_TOUCH_DOWN_COORD_CFG);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "set_sys_config failed (may need RS232 level shifter)");
  }

  topway_set_backlight(0x19);

  topway_set_codepage(1, 12);

  topway_disp_page(PAGE_MAIN);

  /* Register touch event callback */
  topway_register_touch_callback(idms_ui_topway_process_touch_event);

  s_display_ready = true;

  update_display_values();

  /* Send device info to INFO page */
  idms_ui_topway_send_device_info();

  /* Send config parameters to LCD */
  idms_ui_topway_send_config();

  /* One-time clear of WiFi fields at boot to remove LCD default values.
   * This is a workaround - the LCD project should disable auto-refresh
   * and remove default values for VP_STR_WIFI_SSID and VP_STR_WIFI_PASSWORD.
   * We do this AFTER all other init to ensure LCD is fully ready. */
  if (!s_wifi_fields_cleared) {
    ESP_LOGI(TAG, "Clearing WiFi credential fields (one-time workaround)");
    vTaskDelay(pdMS_TO_TICKS(100));  /* Give LCD time to stabilize */
    topway_str_write(VP_STR_WIFI_SSID, "");
    vTaskDelay(pdMS_TO_TICKS(50));
    topway_str_write(VP_STR_WIFI_PASSWORD, "");
    s_wifi_fields_cleared = true;
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

  ESP_LOGI(TAG, "Topway UI ready (800x480)");
  return ESP_OK;
}

void idms_ui_topway_update(void) {
  if (s_ui_task) {
    xTaskNotifyGive(s_ui_task);
  }
}

void idms_ui_topway_send_device_info(void) {
  if (!s_display_ready)
    return;

  ESP_LOGI(TAG, "Sending device info to Topway LCD");

  char device_model[INFO_FIELD_MAX_LEN] = {0};
  char serial_number[INFO_FIELD_MAX_LEN] = {0};
  char manufacture_date[INFO_FIELD_MAX_LEN] = {0};
  char support_email[INFO_FIELD_MAX_LEN] = {0};
  char support_phone[INFO_FIELD_MAX_LEN] = {0};
  char qr_code[INFO_FIELD_MAX_LEN] = {0};

  if (config_get_device_model(device_model, sizeof(device_model)) != ESP_OK) {
    copy_info_cache(device_model, CONFIG_DEFAULT_DEVICE_MODEL);
  }
  topway_str_write(VP_STR_DEVICE_MODEL, device_model);
  copy_info_cache(s_info_device_model, device_model);
  ESP_LOGI(TAG, "Device Model: %s", device_model);

  /* Firmware Version - from OTA/version info */
  const char *fw_version = ota_get_version();
  topway_str_write(VP_STR_FIRMWARE_VERSION, fw_version);
  ESP_LOGI(TAG, "Firmware Version: %s", fw_version);

  /* Hardware Version - from chip info */
  esp_chip_info_t chip_info;
  esp_chip_info(&chip_info);
  char hw_version[32];
#if CONFIG_IDF_TARGET_ESP32S3
  snprintf(hw_version, sizeof(hw_version), "ESP32-S3 Rev.%d", chip_info.revision);
#elif CONFIG_IDF_TARGET_ESP32
  snprintf(hw_version, sizeof(hw_version), "ESP32 Rev.%d", chip_info.revision);
#else
  snprintf(hw_version, sizeof(hw_version), "ESP32-Unknown Rev.%d", chip_info.revision);
#endif
  topway_str_write(VP_STR_HARDWARE_VERSION, hw_version);
  ESP_LOGI(TAG, "Hardware Version: %s", hw_version);

  if (config_get_serial_number(serial_number, sizeof(serial_number)) != ESP_OK ||
      serial_number[0] == '\0') {
    generate_default_serial_number(serial_number, sizeof(serial_number));
    config_set_serial_number(serial_number);
  }
  topway_str_write(VP_STR_SERIAL_NUMBER, serial_number);
  copy_info_cache(s_info_serial_number, serial_number);
  ESP_LOGI(TAG, "Serial Number: %s", serial_number);

  if (config_get_manufacture_date(manufacture_date, sizeof(manufacture_date)) != ESP_OK ||
      manufacture_date[0] == '\0') {
    generate_default_manufacture_date(manufacture_date, sizeof(manufacture_date));
    config_set_manufacture_date(manufacture_date);
  }
  topway_str_write(VP_STR_MANUFACTURE_DATE, manufacture_date);
  copy_info_cache(s_info_manufacture_date, manufacture_date);
  ESP_LOGI(TAG, "Manufacture Date: %s", manufacture_date);

  if (config_get_support_email(support_email, sizeof(support_email)) != ESP_OK) {
    copy_info_cache(support_email, CONFIG_DEFAULT_SUPPORT_EMAIL);
  }
  topway_str_write(VP_STR_SUPPORT_EMAIL, support_email);
  copy_info_cache(s_info_support_email, support_email);
  ESP_LOGI(TAG, "Support Email: %s", support_email);

  if (config_get_support_phone(support_phone, sizeof(support_phone)) != ESP_OK) {
    copy_info_cache(support_phone, CONFIG_DEFAULT_SUPPORT_PHONE);
  }
  topway_str_write(VP_STR_SUPPORT_PHONE, support_phone);
  copy_info_cache(s_info_support_phone, support_phone);
  ESP_LOGI(TAG, "Support Phone: %s", support_phone);

  if (config_get_qr_code(qr_code, sizeof(qr_code)) != ESP_OK) {
    copy_info_cache(qr_code, CONFIG_DEFAULT_QR_CODE);
  }
  topway_str_write(VP_STR_QR_CODE, qr_code);
  copy_info_cache(s_info_qr_code, qr_code);
  ESP_LOGI(TAG, "QR Code: %s", qr_code);
}

void idms_ui_topway_send_config(void) {
  if (!s_display_ready)
    return;

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

  /* Send Min Temp IN threshold (pure Celsius) */
  topway_n16_write(VP_N16_CFG_MIN_TIN, (uint16_t)min_tin);
  ESP_LOGI(TAG, "Min Temp IN: %d C", min_tin);

  /* Send Max Temp IN threshold (pure Celsius) */
  topway_n16_write(VP_N16_CFG_MAX_TIN, (uint16_t)max_tin);
  ESP_LOGI(TAG, "Max Temp IN: %d C", max_tin);

  /* Send Min Temp OUT threshold (pure Celsius) */
  topway_n16_write(VP_N16_CFG_MIN_TOUT, (uint16_t)min_tout);
  ESP_LOGI(TAG, "Min Temp OUT: %d C", min_tout);

  /* Send Max Temp OUT threshold (pure Celsius) */
  topway_n16_write(VP_N16_CFG_MAX_TOUT, (uint16_t)max_tout);
  ESP_LOGI(TAG, "Max Temp OUT: %d C", max_tout);

  /* Send Min Current threshold (pure Amperes) */
  topway_n16_write(VP_N16_CFG_MIN_CURRENT, min_current);
  ESP_LOGI(TAG, "Min Current: %d A", min_current);

  /* Send Max Current threshold (pure Amperes) */
  topway_n16_write(VP_N16_CFG_MAX_CURRENT, max_current);
  ESP_LOGI(TAG, "Max Current: %d A", max_current);

  /* Send Delta_T threshold (pure Celsius) */
  topway_n16_write(VP_N16_CFG_DT_ALERT, (uint16_t)dt_alert);
  ESP_LOGI(TAG, "Delta_T threshold: %d C", dt_alert);
}

void idms_ui_topway_check_wifi_button(void)
{
  if (!s_display_ready)
    return;

  uint16_t wifi_btn = 0;
  
  esp_err_t err = topway_n16_read(VP_N16_WIFI_CONNECT_BTN, &wifi_btn);
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
      if (!wifi_manager_is_connected()) {
        ESP_LOGW(TAG, "WiFi SSID invalid or placeholder: '%s' - clearing fields", ssid);
        topway_str_write(VP_STR_WIFI_STATUS_MSG, "WiFi: Enter SSID/Pass");
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
    if (wifi_manager_is_connected() && 
        strcmp(current_ssid, ssid) == 0 && 
        strcmp(current_pass, password) == 0) {
      ESP_LOGI(TAG, "WiFi already connected with same credentials, skipping reconnect");
      topway_str_write(VP_STR_WIFI_PASSWORD, "");  /* Clear password field */
      return;
    }
    
    /* If connected with same SSID but different password, save but don't reconnect */
    if (wifi_manager_is_connected() && strcmp(current_ssid, ssid) == 0) {
      ESP_LOGI(TAG, "WiFi connected - saving new password for next connection");
      if (config_set_wifi_password(password) == ESP_OK) {
        topway_str_write(VP_STR_WIFI_PASSWORD, "");
        topway_str_write(VP_STR_WIFI_STATUS_MSG, "WiFi: Pass saved");
        ESP_LOGI(TAG, "New password saved. Will use on next connect.");
      }
      return;
    }
    
    /* Validate password is not empty for secure networks */
    if (password[0] == '\0') {
      ESP_LOGW(TAG, "WiFi password is empty - cannot connect to secure network");
      topway_str_write(VP_STR_WIFI_STATUS_MSG, "WiFi: Enter Password");
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
      topway_str_write(VP_STR_WIFI_STATUS_MSG, "WiFi: Changing Network...");
      
      /* Trigger WiFi reconnect */
      wifi_manager_reconnect();
    } else {
      ESP_LOGE(TAG, "Failed to save WiFi credentials to NVS");
      topway_str_write(VP_STR_WIFI_STATUS_MSG, "WiFi: Save Error");
    }
  }
}
