idf_component_register(
  SRCS "telemetry.c"
  INCLUDE_DIRS "include"
  PRIV_REQUIRES
    monitor nvs_flash freertos spiffs
)
