/**
 * @file ui_topway.h
 * @brief LVGL display driver for Topway Smart LCD
 */

#ifndef UI_TOPWAY_H
#define UI_TOPWAY_H

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

#ifdef __cplusplus
}
#endif

#endif /* UI_TOPWAY_H */
