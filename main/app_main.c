#include "esp_log.h"
#include "driver/spi_master.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config_store.h"
#include "wifi_manager.h"
#include "monitor.h"
#include "telemetry.h"
#include "cloud_sync.h"

#if CONFIG_IDMS_DISPLAY_TOPWAY
#include "ui_topway.h"
#else
#include "ui_lvgl.h"
#endif

#include "telegram.h"
#include "ota.h"
#include "serial_console.h"
#include "pins.h"

#if !CONFIG_IDMS_DISPLAY_TOPWAY && CONFIG_IDMS_LCD_BUS_SPI
#include "lcd_diag.h"
#endif

#if CONFIG_IDMS_TEMP_SENSOR_MAX31865
#include "max31865.h"
#endif

#if CONFIG_IDMS_TEMP_SENSOR_DS18B20 || CONFIG_IDMS_TEMP_SENSOR_DS18B20_PT100
#include "ds18b20.h"
#endif

static const char *TAG = "app";

#if CONFIG_IDMS_PRODUCTION_BUILD
#if !CONFIG_SECURE_BOOT
#error "Production build requires CONFIG_SECURE_BOOT=y"
#endif
#if !CONFIG_SECURE_FLASH_ENC_ENABLED
#error "Production build requires CONFIG_SECURE_FLASH_ENC_ENABLED=y"
#endif
#if !CONFIG_NVS_ENCRYPTION
#error "Production build requires CONFIG_NVS_ENCRYPTION=y"
#endif
#endif

#define OTA_HEALTH_INITIAL_DELAY_MS 30000
#define OTA_HEALTH_POLL_MS          5000
#define OTA_HEALTH_TIMEOUT_MS       180000

static bool s_telemetry_ready;
static bool s_ui_ready;
static bool s_telegram_ready;

static bool app_health_ok_for_ota(void)
{
    idms_metrics_t m;
    monitor_get_metrics(&m);

    if (!s_telemetry_ready || !s_ui_ready || !s_telegram_ready) {
        return false;
    }
    if (!m.sensor_preflight_done || !m.sensor_preflight_ok || m.sensor_error_flags != 0) {
        return false;
    }
    if (!m.current_valid || !m.t_in_valid || !m.t_out_valid || !m.delta_valid) {
        return false;
    }
    return true;
}

static void ota_health_validation_task(void *arg)
{
    (void)arg;
    if (!ota_is_pending_validation()) {
        vTaskDelete(NULL);
        return;
    }

    ota_schedule_valid_mark(0);
    ESP_LOGI(TAG, "OTA health validation started");
    vTaskDelay(pdMS_TO_TICKS(OTA_HEALTH_INITIAL_DELAY_MS));

    uint32_t elapsed = OTA_HEALTH_INITIAL_DELAY_MS;
    while (elapsed <= OTA_HEALTH_TIMEOUT_MS) {
        if (app_health_ok_for_ota()) {
            ESP_LOGI(TAG, "OTA health validation passed after %lu ms", (unsigned long)elapsed);
            ota_mark_app_valid();
            vTaskDelete(NULL);
            return;
        }

        idms_metrics_t m;
        monitor_get_metrics(&m);
        ESP_LOGW(TAG,
                 "OTA health pending: telemetry=%s ui=%s telegram=%s sensors=%s flags=0x%02lx current=%s Tin=%s Tout=%s dT=%s",
                 s_telemetry_ready ? "ok" : "fail",
                 s_ui_ready ? "ok" : "fail",
                 s_telegram_ready ? "ok" : "fail",
                 m.sensor_preflight_ok ? "ok" : m.sensor_status,
                 (unsigned long)m.sensor_error_flags,
                 m.current_valid ? "ok" : "invalid",
                 m.t_in_valid ? "ok" : "invalid",
                 m.t_out_valid ? "ok" : "invalid",
                 m.delta_valid ? "ok" : "invalid");

        vTaskDelay(pdMS_TO_TICKS(OTA_HEALTH_POLL_MS));
        elapsed += OTA_HEALTH_POLL_MS;
    }

    ota_mark_app_invalid_and_reboot();
    vTaskDelete(NULL);
}

#if !CONFIG_IDMS_DISPLAY_TOPWAY && CONFIG_IDMS_LCD_BUS_SPI
static void init_lcd_spi_bus(void)
{
    spi_bus_config_t buscfg = {
        .sclk_io_num = CONFIG_IDMS_PIN_LCD_SCLK,
        .mosi_io_num = CONFIG_IDMS_PIN_LCD_MOSI,
#if CONFIG_IDMS_PIN_LCD_MISO >= 0
        .miso_io_num = CONFIG_IDMS_PIN_LCD_MISO,
#else
        .miso_io_num = -1,
#endif
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
#if CONFIG_IDMS_DISPLAY_ST7796S
        .max_transfer_sz = 480 * 20 * sizeof(uint16_t),
#elif CONFIG_IDMS_DISPLAY_ILI9488
        .max_transfer_sz = 480 * 60 * sizeof(uint16_t),
#elif CONFIG_IDMS_DISPLAY_ILI9341
        .max_transfer_sz = 320 * 40 * sizeof(uint16_t),
#else
        .max_transfer_sz = 4096,
#endif
    };
    ESP_ERROR_CHECK(spi_bus_initialize(IDMS_LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));
    ESP_LOGI(TAG, "LCD SPI bus initialized (host=%d, SCLK=%d, MOSI=%d)",
             (int)IDMS_LCD_SPI_HOST, (int)CONFIG_IDMS_PIN_LCD_SCLK,
             (int)CONFIG_IDMS_PIN_LCD_MOSI);
}
#endif

#if CONFIG_IDMS_TEMP_SENSOR_MAX31865
static void init_max31865_spi_bus(void)
{
    spi_bus_config_t buscfg = {
        .sclk_io_num = CONFIG_IDMS_PIN_MAX31865_SCLK,
        .mosi_io_num = CONFIG_IDMS_PIN_MAX31865_MOSI,
        .miso_io_num = CONFIG_IDMS_PIN_MAX31865_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(IDMS_SENSOR_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));
    ESP_LOGI(TAG, "MAX31865 SPI bus initialized (host=%d, SCLK=%d, MOSI=%d, MISO=%d)",
             (int)IDMS_SENSOR_SPI_HOST,
             (int)CONFIG_IDMS_PIN_MAX31865_SCLK,
             (int)CONFIG_IDMS_PIN_MAX31865_MOSI,
             (int)CONFIG_IDMS_PIN_MAX31865_MISO);
}
#endif

#if CONFIG_IDMS_PIN_TOUCH_CS >= 0
#if !defined(IDMS_LCD_SPI_HOST)
/* Touch gets its own SPI3 bus (no SPI LCD sharing it) */
static void init_touch_spi_bus(void)
{
    spi_bus_config_t buscfg = {
        .sclk_io_num = CONFIG_IDMS_PIN_TOUCH_SCLK,
        .mosi_io_num = CONFIG_IDMS_PIN_TOUCH_MOSI,
        .miso_io_num = CONFIG_IDMS_PIN_TOUCH_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(IDMS_TOUCH_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));
    ESP_LOGI(TAG, "Touch SPI bus initialized (host=%d, SCLK=%d, MOSI=%d, MISO=%d)",
             (int)IDMS_TOUCH_SPI_HOST,
             (int)CONFIG_IDMS_PIN_TOUCH_SCLK,
             (int)CONFIG_IDMS_PIN_TOUCH_MOSI,
             (int)CONFIG_IDMS_PIN_TOUCH_MISO);
}
#else
/* Touch shares SPI bus with LCD — no separate init needed */
static void init_touch_spi_bus(void) { }
#endif
#endif

void app_main(void)
{
    ESP_LOGI(TAG, "ESP-IDMS firmware boot");

    ESP_ERROR_CHECK(config_store_init());
    ESP_ERROR_CHECK(wifi_manager_init());

#if !CONFIG_IDMS_DISPLAY_TOPWAY && CONFIG_IDMS_LCD_BUS_SPI
    uint32_t lcd_id = lcd_diag_read_rddi(
        CONFIG_IDMS_PIN_LCD_SCLK,
        CONFIG_IDMS_PIN_LCD_MOSI,
#if CONFIG_IDMS_PIN_LCD_MISO >= 0
        CONFIG_IDMS_PIN_LCD_MISO,
#else
        -1,
#endif
        CONFIG_IDMS_PIN_LCD_CS,
        CONFIG_IDMS_PIN_LCD_DC,
        CONFIG_IDMS_PIN_LCD_RST);
    lcd_diag_print_rddi(lcd_id);
#elif !CONFIG_IDMS_DISPLAY_TOPWAY && CONFIG_IDMS_LCD_BUS_I80
    ESP_LOGI(TAG, "I80 mode: skipping RDDID diagnostic");
#endif

#if !CONFIG_IDMS_DISPLAY_TOPWAY && CONFIG_IDMS_LCD_BUS_SPI
    init_lcd_spi_bus();
#endif

#if CONFIG_IDMS_TEMP_SENSOR_MAX31865
    init_max31865_spi_bus();
#endif

#if CONFIG_IDMS_PIN_TOUCH_CS >= 0
    init_touch_spi_bus();
#endif

    esp_err_t monitor_err = monitor_init();
    if (monitor_err != ESP_OK) {
        ESP_LOGE(TAG, "Monitor sensor preflight reported errors: %s", esp_err_to_name(monitor_err));
    }

    esp_err_t telemetry_err = telemetry_init();
    if (telemetry_err != ESP_OK) {
        ESP_LOGE(TAG, "Telemetry init failed: %s", esp_err_to_name(telemetry_err));
        s_telemetry_ready = false;
    } else {
        s_telemetry_ready = true;
    }

    esp_err_t cloud_err = cloud_sync_init();
    if (cloud_err != ESP_OK) {
        ESP_LOGE(TAG, "Cloud sync init failed: %s", esp_err_to_name(cloud_err));
    }

#if CONFIG_IDMS_DISPLAY_TOPWAY
    esp_err_t ui_err = idms_ui_topway_init();
    if (ui_err != ESP_OK) {
        ESP_LOGE(TAG, "Topway UI init failed: %s", esp_err_to_name(ui_err));
        s_ui_ready = false;
    } else {
        s_ui_ready = true;
    }
#else
    idms_ui_init();
    s_ui_ready = true;
#endif

    esp_err_t telegram_err = telegram_command_poll_start();
    if (telegram_err != ESP_OK) {
        ESP_LOGE(TAG, "Telegram init failed: %s", esp_err_to_name(telegram_err));
        s_telegram_ready = false;
    } else {
        s_telegram_ready = true;
    }
    serial_console_start();

    ESP_ERROR_CHECK(ota_init());
    if (ota_is_pending_validation()) {
        BaseType_t ok = xTaskCreatePinnedToCore(ota_health_validation_task, "ota_health",
                                                4096, NULL, 4, NULL, tskNO_AFFINITY);
        if (ok != pdPASS) {
            ESP_LOGE(TAG, "Failed to create OTA health task");
            ota_mark_app_invalid_and_reboot();
        }
    }

    ESP_LOGI(TAG, "All subsystems started (version: %s)", ota_get_version());
}
