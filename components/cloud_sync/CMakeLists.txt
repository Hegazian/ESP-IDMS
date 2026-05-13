idf_component_register(
  SRCS "cloud_sync.c"
  INCLUDE_DIRS "include"
  PRIV_REQUIRES core telemetry wifi esp_http_client esp-tls nvs_flash freertos
)
