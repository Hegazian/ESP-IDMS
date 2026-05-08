set(priv_requires
    esp_app_format
    esp_partition
    spi_flash
    app_update
    bootloader_support
    esp_http_server
    esp_https_server
    nvs_flash
    telegram
    telemetry
    mbedtls
    core
    freertos
)

idf_component_register(
    SRCS "ota.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES ${priv_requires}
)

if(CONFIG_IDMS_OTA_HTTPS_ENABLE)
    target_add_binary_data(${COMPONENT_LIB}
        "certs/ota_server_cert.pem" BINARY)
    target_add_binary_data(${COMPONENT_LIB}
        "certs/ota_server_key.pem" BINARY)
endif()
