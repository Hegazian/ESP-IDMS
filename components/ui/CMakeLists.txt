set(ui_requires
    esp_lcd
    lvgl
    driver
    esp_timer
    ota
    core
    monitor
    drivers
)

if(CONFIG_IDMS_DISPLAY_ILI9341)
    list(APPEND ui_requires esp_lcd_ili9341)
endif()

if(CONFIG_IDMS_DISPLAY_ST7796S OR CONFIG_IDMS_DISPLAY_ILI9488)
    list(APPEND ui_requires esp_psram)
endif()

idf_component_register(
    SRCS "ui_lvgl.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES ${ui_requires}
)