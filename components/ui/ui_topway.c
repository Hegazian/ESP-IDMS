/**
 * @file ui_topway.c
 * @brief ESP-IDMS UI for Topway Smart LCD using VP memory protocol
 * 
 * This UI uses Topway's VP (Variable Pointer) memory to update display values.
 * The ESP32 writes to predefined VP addresses to update metrics on screen.
 */

#include "sdkconfig.h"
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

static const char *TAG = "ui_topway";

/* Display layout - adjust these to match your Topway project */
#define PAGE_MAIN               0

/* Text positions for labels (adjust based on your Topway screen design) */
#define LABEL_X                 30
#define TITLE_Y                 30
#define DATA_Y_START            100
#define DATA_ROW_HEIGHT         50

static TimerHandle_t s_ui_timer = NULL;

/**
 * @brief Update all display values via VP memory writes
 */
static void update_display_values(void)
{
    idms_metrics_t m;
    monitor_get_metrics(&m);
    
    char buf[32];
    
    /* Update current value */
    if (m.current_valid) {
        topway_write_float(VP_CURRENT_VALUE, m.current_a);
        topway_write_vp16(VP_CURRENT_VALID, 1);
    } else {
        topway_write_vp16(VP_CURRENT_VALID, 0);
    }
    
    /* Update T_in */
    if (m.t_in_valid) {
        topway_write_float(VP_TIN_VALUE, m.t_in_c);
        topway_write_vp16(VP_TIN_VALID, 1);
    } else {
        topway_write_vp16(VP_TIN_VALID, 0);
    }
    
    /* Update T_out */
    if (m.t_out_valid) {
        topway_write_float(VP_TOUT_VALUE, m.t_out_c);
        topway_write_vp16(VP_TOUT_VALID, 1);
    } else {
        topway_write_vp16(VP_TOUT_VALID, 0);
    }
    
    /* Update Delta T */
    if (m.delta_valid) {
        topway_write_float(VP_DELTA_T_VALUE, m.delta_t_c);
        topway_write_vp16(VP_DELTA_T_VALID, 1);
        
        /* Check for cooling fault */
        if (m.delta_t_c < CONFIG_IDMS_DT_LOW_C || m.delta_t_c > CONFIG_IDMS_DT_HIGH_C) {
            topway_write_vp16(VP_COOL_FAULT, 1);
        } else {
            topway_write_vp16(VP_COOL_FAULT, 0);
        }
    } else {
        topway_write_vp16(VP_DELTA_T_VALID, 0);
    }
    
    /* Update WiFi status */
#if CONFIG_IDMS_UI_ENABLE
    if (wifi_manager_is_connected()) {
        topway_write_vp16(VP_WIFI_STATUS, WIFI_CONNECTED);
        /* Write IP address to string variable */
        topway_write_string(VP_TEXT_BUFFER, m.wifi_ip[0] ? m.wifi_ip : "Connected");
    } else {
        topway_write_vp16(VP_WIFI_STATUS, WIFI_OFFLINE);
        topway_write_string(VP_TEXT_BUFFER, "Offline");
    }
#endif
    
    /* Update OTA status */
    const char *ota_status = ota_get_status();
    if (strcmp(ota_status, "ready") == 0) {
        topway_write_vp16(VP_OTA_STATUS, OTA_READY);
    } else if (strcmp(ota_status, "updating") == 0) {
        topway_write_vp16(VP_OTA_STATUS, OTA_UPDATING);
    } else {
        topway_write_vp16(VP_OTA_STATUS, OTA_ERROR);
    }
    
    /* Update technician count */
    topway_write_vp16(VP_TECH_COUNT, config_get_tech_count());
    
    /* Update power fault status */
    if (m.current_valid && m.current_a < (CONFIG_IDMS_CURRENT_THRESHOLD_MA / 1000.0f)) {
        topway_write_vp16(VP_POWER_FAULT, 1);
    } else {
        topway_write_vp16(VP_POWER_FAULT, 0);
    }
    
    /* Write version to string buffer */
    topway_write_string(VP_TEXT_BUFFER + 0x100, ota_get_version());
}

/**
 * @brief UI timer callback
 */
static void ui_timer_callback(TimerHandle_t timer)
{
    update_display_values();
}

/**
 * @brief Draw static UI elements
 */
static void draw_static_ui(void)
{
    /* Clear screen to dark background */
    topway_clear_screen(TOPWAY_COLOR_BLACK);
    
    /* Draw title */
    topway_draw_text(250, TITLE_Y, "ESP-IDMS Monitor", TOPWAY_COLOR_CYAN, 2);
    
    /* Draw separator line */
    topway_draw_line(30, TITLE_Y + 45, 770, TITLE_Y + 45, TOPWAY_COLOR_CYAN);
    
    /* Draw labels */
    int y = DATA_Y_START;
    
    topway_draw_text(LABEL_X, y, "Current:", TOPWAY_COLOR_WHITE, 1);
    topway_draw_text(LABEL_X, y + DATA_ROW_HEIGHT, "T_in:", TOPWAY_COLOR_WHITE, 1);
    topway_draw_text(LABEL_X, y + DATA_ROW_HEIGHT*2, "T_out:", TOPWAY_COLOR_WHITE, 1);
    topway_draw_text(LABEL_X, y + DATA_ROW_HEIGHT*3, "Delta T:", TOPWAY_COLOR_WHITE, 1);
    topway_draw_text(LABEL_X, y + DATA_ROW_HEIGHT*4, "WiFi:", TOPWAY_COLOR_WHITE, 1);
    topway_draw_text(LABEL_X, y + DATA_ROW_HEIGHT*5, "OTA:", TOPWAY_COLOR_WHITE, 1);
    topway_draw_text(LABEL_X, y + DATA_ROW_HEIGHT*6, "Technicians:", TOPWAY_COLOR_WHITE, 1);
    topway_draw_text(LABEL_X, y + DATA_ROW_HEIGHT*7, "Version:", TOPWAY_COLOR_WHITE, 1);
    
    /* Draw footer */
    topway_draw_text(200, 430, "Type 'help' in serial console", TOPWAY_COLOR_GRAY, 0);
    
    /* Draw status indicators */
    topway_fill_rect(600, DATA_Y_START + DATA_ROW_HEIGHT*4, 30, 20, TOPWAY_COLOR_RED);  /* WiFi LED */
    topway_fill_rect(600, DATA_Y_START + DATA_ROW_HEIGHT*5, 30, 20, TOPWAY_COLOR_GREEN); /* OTA LED */
}

esp_err_t idms_ui_topway_init(void)
{
    ESP_LOGI(TAG, "Initializing Topway UI (800x480)");
    
    /* Configure Topway display */
    topway_config_t config = {
        .uart_port = UART_NUM_1,
        .tx_pin = CONFIG_IDMS_PIN_UART_TX,
        .rx_pin = CONFIG_IDMS_PIN_UART_RX,
        .baud_rate = 115200,
        .width = 800,
        .height = 480,
    };
    
    esp_err_t err = topway_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Topway init failed: %s", esp_err_to_name(err));
        return err;
    }
    
    /* Set brightness */
    topway_set_brightness(80);
    
    /* Draw static UI elements */
    draw_static_ui();
    
    /* Set to main page */
    topway_set_page(PAGE_MAIN);
    
    /* Initial update */
    update_display_values();
    
    /* Create timer for periodic updates (500ms) */
    s_ui_timer = xTimerCreate(
        "ui_topway",
        pdMS_TO_TICKS(500),
        pdTRUE,
        NULL,
        ui_timer_callback
    );
    
    if (s_ui_timer) {
        xTimerStart(s_ui_timer, 0);
    }
    
    ESP_LOGI(TAG, "Topway UI ready (800x480)");
    return ESP_OK;
}

void idms_ui_topway_update(void)
{
    if (s_ui_timer) {
        xTimerReset(s_ui_timer, 0);
    }
}
