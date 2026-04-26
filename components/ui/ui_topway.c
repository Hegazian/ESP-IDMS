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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include <string.h>

static const char *TAG = "ui_topway";

#define PAGE_MAIN 0

static TimerHandle_t s_ui_timer = NULL;
static bool s_display_ready = false;
static float s_last_current = -999.0f;
static float s_last_tin = -999.0f;
static float s_last_tout = -999.0f;
static float s_last_dt = -999.0f;
static const char *s_last_diag = "";

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
    if (m.delta_t_c < CONFIG_IDMS_DT_LOW_C ||
        m.delta_t_c > CONFIG_IDMS_DT_HIGH_C) {
      topway_n16_write(VP_N16_COOL_FAULT, 1);
    } else {
      topway_n16_write(VP_N16_COOL_FAULT, 0);
    }
  } else {
    topway_n16_write(VP_N16_DT_VALID, 0);
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

  if (m.current_valid &&
      m.current_a < (CONFIG_IDMS_CURRENT_THRESHOLD_MA / 1000.0f)) {
    topway_n16_write(VP_N16_POWER_FAULT, 1);
  } else {
    topway_n16_write(VP_N16_POWER_FAULT, 0);
  }

  topway_str_write(VP_STR_VERSION, ota_get_version());

  /* Determine device status and diagnostic message */
  const char *status_str = "ACTIVE";
  const char *diag_str = "OK";

  bool has_power_fault = (m.current_valid &&
      m.current_a < (CONFIG_IDMS_CURRENT_THRESHOLD_MA / 1000.0f));
  bool has_cool_fault = (m.delta_valid &&
      (m.delta_t_c < CONFIG_IDMS_DT_LOW_C || m.delta_t_c > CONFIG_IDMS_DT_HIGH_C));
  bool temp_invalid = (!m.t_in_valid || !m.t_out_valid);
  bool current_invalid = !m.current_valid;

  if (temp_invalid && current_invalid) {
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

static void ui_timer_callback(TimerHandle_t timer) { update_display_values(); }

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

  err = topway_set_sys_config(TOPWAY_BAUD_9600, TOPWAY_TOUCH_DOWN_COORD_CFG);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "set_sys_config failed (may need RS232 level shifter)");
  }

  topway_set_backlight(0x19);

  topway_set_codepage(1, 12);

  topway_disp_page(PAGE_MAIN);

  s_display_ready = true;

  update_display_values();

  s_ui_timer = xTimerCreate("ui_topway", pdMS_TO_TICKS(500), pdTRUE, NULL,
                            ui_timer_callback);

  if (s_ui_timer) {
    xTimerStart(s_ui_timer, 0);
  }

  ESP_LOGI(TAG, "Topway UI ready (800x480)");
  return ESP_OK;
}

void idms_ui_topway_update(void) {
  if (s_ui_timer) {
    xTimerReset(s_ui_timer, 0);
  }
}
