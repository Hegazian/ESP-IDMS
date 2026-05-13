# ESP-IDMS — Application entry point
idf_component_register(
  SRCS "app_main.c"
  INCLUDE_DIRS "."
  PRIV_REQUIRES
    core wifi telegram monitor telemetry cloud_sync ota ui cli drivers
)
