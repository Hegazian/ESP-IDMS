# ESP-IDMS — Application entry point
idf_component_register(
  SRCS "app_main.c"
  INCLUDE_DIRS "."
  PRIV_REQUIRES
    core network telegram monitor telemetry cloud_sync ota ui cli drivers esp_driver_spi esp_driver_gpio mqtt_service modbus_service
)
