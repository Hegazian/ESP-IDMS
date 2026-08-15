idf_component_register(
  SRCS "serial_console.c"
  INCLUDE_DIRS "include"
  PRIV_REQUIRES core ota monitor network nvs_flash drivers
)
