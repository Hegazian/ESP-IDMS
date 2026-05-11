idf_component_register(
  SRCS "config_store.c"
  INCLUDE_DIRS "include"
  PRIV_REQUIRES nvs_flash mbedtls esp_hw_support
)
