set(ui_srcs "")
set(ui_requires
    driver
    esp_timer
    ota
    core
    monitor
    drivers
    wifi
)

if(CONFIG_IDMS_DISPLAY_TOPWAY)
    # Topway uses native UI (no LVGL)
    list(APPEND ui_srcs "ui_topway.c")
    list(APPEND ui_requires uart)
else()
    # LVGL-based displays
    list(APPEND ui_srcs "ui_lvgl.c")
    list(APPEND ui_requires esp_lcd lvgl)
    
    if(CONFIG_IDMS_DISPLAY_ILI9341)
        list(APPEND ui_requires esp_lcd_ili9341)
    endif()
    
    if(CONFIG_IDMS_DISPLAY_ST7796S OR CONFIG_IDMS_DISPLAY_ILI9488)
        list(APPEND ui_requires esp_psram)
    endif()
endif()

idf_component_register(
    SRCS ${ui_srcs}
    INCLUDE_DIRS "include"
    PRIV_REQUIRES ${ui_requires}
)