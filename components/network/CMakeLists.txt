idf_component_register(
  SRCS "network_manager.c"
  INCLUDE_DIRS "include"
  PRIV_REQUIRES esp_netif esp_wifi esp_event nvs_flash core esp_eth esp_driver_gpio esp_driver_spi
)
