set(driver_srcs "xpt2046.c" "lcd_diag.c")

if(CONFIG_IDMS_TEMP_SENSOR_DS18B20)
    list(APPEND driver_srcs "onewire.c" "ds18b20.c")
endif()

if(CONFIG_IDMS_TEMP_SENSOR_MAX31865)
    list(APPEND driver_srcs "max31865.c")
endif()

if(CONFIG_IDMS_DISPLAY_ST7796S)
    list(APPEND driver_srcs "esp_lcd_st7796s.c")
endif()

if(CONFIG_IDMS_DISPLAY_ILI9488)
    list(APPEND driver_srcs "esp_lcd_ili9488.c")
endif()

idf_component_register(
    SRCS ${driver_srcs}
    INCLUDE_DIRS "include"
    PRIV_REQUIRES driver esp_lcd
)