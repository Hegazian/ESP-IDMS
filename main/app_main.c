#include "esp_log.h"
#include "driver/spi_master.h"
#include "sdkconfig.h"

#include "config_store.h"
#include "wifi_manager.h"
#include "monitor.h"

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

#if CONFIG_IDMS_DISPLAY_TOPWAY
    idms_ui_topway_init();
#else
    idms_ui_init();
#endif

    telegram_command_poll_start();
    serial_console_start();

    ESP_ERROR_CHECK(ota_init());
    ota_schedule_valid_mark(30000);

    ESP_LOGI(TAG, "All subsystems started (version: %s)", ota_get_version());
}
