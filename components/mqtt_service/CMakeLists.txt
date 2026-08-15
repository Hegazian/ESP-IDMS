idf_component_register(
    SRCS "mqtt_service.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES core monitor telemetry network mqtt json
)
