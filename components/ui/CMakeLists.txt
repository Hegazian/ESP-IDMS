idf_component_register(
  SRCS "ui_lvgl.c"
  INCLUDE_DIRS "include"
  PRIV_REQUIRES esp_lcd esp_lcd_ili9341 lvgl driver esp_timer esp_psram
    ota core monitor drivers
)
