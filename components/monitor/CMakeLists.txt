idf_component_register(
  SRCS "monitor.c"
  INCLUDE_DIRS "include"
  PRIV_REQUIRES driver esp_adc telegram core wifi drivers
)
