idf_component_register(
  SRCS
    "app_main.c"
    "onewire.c"
    "ds18b20.c"
    "config_store.c"
    "wifi_manager.c"
    "telegram.c"
    "monitor.c"
    "xpt2046.c"
    "ui_lvgl.c"
    "ota.c"
  INCLUDE_DIRS "."
  PRIV_REQUIRES
    nvs_flash
    esp_netif
    esp_wifi
    esp_event
    esp_http_client
    esp_http_server
    esp_timer
    driver
    esp_adc
    esp_lcd
    mbedtls
    esp_lcd_ili9341
    lvgl
    esp_app_format
    esp_partition
    spi_flash
    app_update
    bootloader_support
    lwip
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE PROJECT_VER=\"${PROJECT_VER}\")
