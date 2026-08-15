idf_component_register(
  SRCS "tg_http.c" "tg_send.c" "tg_parse.c" "tg_ui.c" "tg_bot.c" "tg_token.c" "telegram.c"
  INCLUDE_DIRS "include"
  PRIV_REQUIRES
    esp_http_client mbedtls nvs_flash freertos lwip json
    core network monitor telemetry ota
)
