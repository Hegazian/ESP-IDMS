# ESP-IDMS — Application entry point
idf_component_register(
  SRCS "app_main.c"
  INCLUDE_DIRS "."
  PRIV_REQUIRES
    core wifi telegram monitor ota ui cli drivers
)
