/**
 * @file ui_topway.h
 * @brief LVGL display driver for Topway Smart LCD
 */

#ifndef UI_TOPWAY_H
#define UI_TOPWAY_H

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize UI with Topway display
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t idms_ui_topway_init(void);

/**
 * @brief Get UI metrics for update
 */
void idms_ui_topway_update(void);

/**
 * @brief Send device information to Topway LCD display
 * 
 * Sends device model, firmware version, hardware version, 
 * serial number, and manufacture date to the INFO page VP addresses.
 */
void idms_ui_topway_send_device_info(void);

/**
 * @brief Send configuration parameters to Topway LCD display
 * 
 * Sends Min/Max Temp IN/OUT thresholds and Min/Max Current thresholds
 * to the configuration VP addresses for display on the LCD.
 */
void idms_ui_topway_send_config(void);

/**
 * @brief Read configuration parameters from Topway LCD display
 * 
 * Reads the configuration values from the LCD VP addresses.
 * Useful when user modifies settings on the LCD touchscreen.
 * 
 * @param min_tin     Pointer to store Min Temp IN (x10, 0.1C precision), or NULL
 * @param min_tout    Pointer to store Min Temp OUT (x10, 0.1C precision), or NULL
 * @param min_current Pointer to store Min Current (mA), or NULL
 * @param max_tin     Pointer to store Max Temp IN (x10, 0.1C precision), or NULL
 * @param max_tout    Pointer to store Max Temp OUT (x10, 0.1C precision), or NULL
 * @param max_current Pointer to store Max Current (mA), or NULL
 * @return esp_err_t ESP_OK on success, error code otherwise
 */
esp_err_t idms_ui_topway_read_config(int16_t *min_tin, int16_t *min_tout, uint16_t *min_current,
                                      int16_t *max_tin, int16_t *max_tout, uint16_t *max_current);

/**
 * @brief Check if apply button was pressed and read config from LCD
 * 
 * This function should be called periodically (e.g., from timer callback).
 * It monitors the apply button VP and when pressed, reads all config
 * values from the LCD and stores them internally.
 */
void idms_ui_topway_check_apply_button(void);

/**
 * @brief Process touch event from LCD
 * 
 * Called when a touch/key event is received from the Topway LCD.
 * Use this to detect button presses.
 * 
 * @param page_id Page ID where touch occurred
 * @param key_id  Key/element ID that was touched
 */
void idms_ui_topway_process_touch_event(uint8_t page_id, uint8_t key_id);

/**
 * @brief Get the applied configuration values from LCD
 * 
 * Returns the config values that were last applied via the LCD apply button.
 * Returns false if no config has been applied yet.
 * 
 * @param min_tin     Pointer to store Min Temp IN (C), or NULL
 * @param min_tout    Pointer to store Min Temp OUT (C), or NULL
 * @param min_current Pointer to store Min Current (A), or NULL
 * @param max_tin     Pointer to store Max Temp IN (C), or NULL
 * @param max_tout    Pointer to store Max Temp OUT (C), or NULL
 * @param max_current Pointer to store Max Current (A), or NULL
 * @return true if config has been applied, false otherwise
 */
bool idms_ui_topway_get_config(int16_t *min_tin, int16_t *min_tout, uint16_t *min_current,
                                int16_t *max_tin, int16_t *max_tout, uint16_t *max_current);

/**
 * @brief Check WiFi connect button and update credentials
 * 
 * Monitors the WiFi connect button VP (0x080020). When pressed,
 * reads SSID and password from LCD, saves to NVS, and triggers reconnect.
 * This function should be called periodically.
 */
void idms_ui_topway_check_wifi_button(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_TOPWAY_H */
