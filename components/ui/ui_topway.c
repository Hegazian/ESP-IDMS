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

static void update_display_values(void) {
  if (!s_display_ready)
    return;

  idms_metrics_t m;
  monitor_get_metrics(&m);

  if (m.current_valid) {
    topway_n16_write(VP_N16_CUR_X10, (uint16_t)(m.current_a * 10.0f + 0.5f));
    topway_n16_write(VP_N16_CUR_VALID, 1);
  } else {
    topway_n16_write(VP_N16_CUR_VALID, 0);
    topway_str_write(VP_STR_CURRENT, "--");
  }

  if (m.t_in_valid) {
    topway_n16_write(VP_N16_TIN_X10, (uint16_t)(m.t_in_c * 10.0f + 0.5f));
    topway_n16_write(VP_N16_TIN_VALID, 1);
  } else {
    topway_n16_write(VP_N16_TIN_VALID, 0);
  }

  if (m.t_out_valid) {
    topway_n16_write(VP_N16_TOUT_X10, (uint16_t)(m.t_out_c * 10.0f + 0.5f));
    topway_n16_write(VP_N16_TOUT_VALID, 1);
  } else {
    topway_n16_write(VP_N16_TOUT_VALID, 0);
  }

  if (m.delta_valid) {
    int16_t dt_x10 = (int16_t)(m.delta_t_c * 10.0f + 0.5f);
    topway_n16_write(VP_N16_DT_X10, (uint16_t)dt_x10);
    topway_n16_write(VP_N16_DT_VALID, 1);
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
