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

#if CONFIG_IDMS_LCD_BUS_SPI
#include "lcd_diag.h"
#endif

#if CONFIG_IDMS_TEMP_SENSOR_DS18B20
#include "ds18b20.h"
#elif CONFIG_IDMS_TEMP_SENSOR_MAX31865
#include "max31865.h"
#endif

static const char *TAG = "app";

static void init_spi_bus(void)
{
#if CONFIG_IDMS_LCD_BUS_SPI
    int miso_pin = CONFIG_IDMS_PIN_LCD_MISO;
#else
    int miso_pin = -1;  /* I80 mode doesn't use SPI MISO */
#endif

    spi_bus_config_t buscfg = {
        .sclk_io_num = CONFIG_IDMS_PIN_LCD_SCLK,
        .mosi_io_num = CONFIG_IDMS_PIN_LCD_MOSI,
        .miso_io_num = miso_pin,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
#if CONFIG_IDMS_DISPLAY_ST7796S || CONFIG_IDMS_DISPLAY_ILI9488
        .max_transfer_sz = 480 * 50 * sizeof(uint16_t),
#elif CONFIG_IDMS_DISPLAY_ILI9341
        .max_transfer_sz = 320 * 40 * sizeof(uint16_t),
#else
        .max_transfer_sz = 4096,
#endif
    };
#if CONFIG_IDMS_LCD_BUS_I80
    buscfg.max_transfer_sz = 4096;
#endif
    ESP_ERROR_CHECK(spi_bus_initialize(IDMS_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));
#if CONFIG_IDMS_LCD_BUS_SPI
    ESP_LOGI(TAG, "SPI bus initialized (host=%d, SCLK=%d, MOSI=%d, MISO=%d)",
             (int)IDMS_SPI_HOST, (int)CONFIG_IDMS_PIN_LCD_SCLK,
             (int)CONFIG_IDMS_PIN_LCD_MOSI, (int)CONFIG_IDMS_PIN_LCD_MISO);
#else
    ESP_LOGI(TAG, "SPI bus initialized (host=%d, SCLK=%d, MOSI=%d, I80 mode)",
             (int)IDMS_SPI_HOST, (int)CONFIG_IDMS_PIN_LCD_SCLK,
             (int)CONFIG_IDMS_PIN_LCD_MOSI);
#endif
}

void app_main(void)
{
    ESP_LOGI(TAG, "ESP-IDMS firmware boot");

    ESP_ERROR_CHECK(config_store_init());
    ESP_ERROR_CHECK(wifi_manager_init());

/* SPI bus initialization for sensors (MAX31865, touch, etc.) */
#if CONFIG_IDMS_TEMP_SENSOR_DS18B20 || CONFIG_IDMS_TEMP_SENSOR_MAX31865 || CONFIG_IDMS_PIN_TOUCH_CS >= 0
    init_spi_bus();
#endif

/* LCD diagnostics only for non-Topway displays */
#if !CONFIG_IDMS_DISPLAY_TOPWAY
#if CONFIG_IDMS_LCD_BUS_SPI
    /* Read LCD controller ID via bit-bang BEFORE SPI bus init */
    uint32_t lcd_id = lcd_diag_read_rddi(
        CONFIG_IDMS_PIN_LCD_SCLK,
        CONFIG_IDMS_PIN_LCD_MOSI,
        CONFIG_IDMS_PIN_LCD_MISO,
        CONFIG_IDMS_PIN_LCD_CS,
        CONFIG_IDMS_PIN_LCD_DC,
        CONFIG_IDMS_PIN_LCD_RST);
    lcd_diag_print_rddi(lcd_id);
#elif CONFIG_IDMS_LCD_BUS_I80
    /* Skip slow I80 diagnostic - LCD will be initialized by UI driver */
    ESP_LOGI(TAG, "I80 mode: skipping RDDID diagnostic (LCD will init in UI driver)");
#endif
#endif /* !CONFIG_IDMS_DISPLAY_TOPWAY */

#if CONFIG_IDMS_TEMP_SENSOR_DS18B20
    ds18b20_init();
#elif CONFIG_IDMS_TEMP_SENSOR_MAX31865
    max31865_init(IDMS_SPI_HOST);
#endif

    monitor_init();
    
#if CONFIG_IDMS_DISPLAY_TOPWAY
    idms_ui_topway_init();
#else
    idms_ui_init();
#endif

    telegram_command_poll_start();
    serial_console_start();

    ESP_ERROR_CHECK(ota_init());
    ota_mark_app_valid();

    ESP_LOGI(TAG, "All subsystems started (version: %s)", ota_get_version());
}