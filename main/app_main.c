#include "esp_log.h"

#include "config_store.h"
#include "wifi_manager.h"
#include "ds18b20.h"
#include "monitor.h"
#include "ui_lvgl.h"
#include "telegram.h"
#include "ota.h"

static const char *TAG = "app";

void app_main(void)
{
    ESP_LOGI(TAG, "ESP-IDMS firmware boot");

    ESP_ERROR_CHECK(config_store_init());
    ESP_ERROR_CHECK(wifi_manager_init());

    ds18b20_init();
    monitor_init();
    idms_ui_init();

    /* Telegram command polling (OTA commands, future commands) */
    telegram_command_poll_start();

    /* OTA init starts the HTTP server — call after Wi-Fi so we can log the IP */
    ESP_ERROR_CHECK(ota_init());

    /* Mark the app as valid if this is a post-OTA first boot.
     * The bootloader sets a pending-verify flag on OTA boot.
     * If this code runs successfully, rollback is cancelled. */
    ota_mark_app_valid();

    ESP_LOGI(TAG, "All subsystems started (version: %s)", ota_get_version());
}
