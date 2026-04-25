set(driver_srcs "")
set(driver_requires driver)

if(CONFIG_IDMS_TEMP_SENSOR_DS18B20 OR CONFIG_IDMS_TEMP_SENSOR_DS18B20_PT100)
    list(APPEND driver_srcs "onewire.c" "ds18b20.c")
endif()

if(CONFIG_IDMS_TEMP_SENSOR_MAX31865)
    list(APPEND driver_srcs "max31865.c")
endif()

if(CONFIG_IDMS_TEMP_SENSOR_PT100_ADC OR CONFIG_IDMS_TEMP_SENSOR_DS18B20_PT100)
    list(APPEND driver_srcs "adc_pt100.c")
endif()

if(CONFIG_IDMS_DISPLAY_ST7796S OR CONFIG_IDMS_DISPLAY_ILI9488 OR CONFIG_IDMS_DISPLAY_ILI9341)
    list(APPEND driver_srcs "lcd_diag.c")
endif()

if(CONFIG_IDMS_DISPLAY_ST7796S)
    list(APPEND driver_srcs "esp_lcd_st7796s.c")
endif()

if(CONFIG_IDMS_DISPLAY_ILI9488)
    list(APPEND driver_srcs "esp_lcd_ili9488.c")
endif()

if(CONFIG_IDMS_DISPLAY_TOPWAY)
    list(APPEND driver_srcs "topway_lcd.c")
endif()

if(CONFIG_IDMS_PIN_TOUCH_CS GREATER_EQUAL 0)
    list(APPEND driver_srcs "xpt2046.c")
endif()

idf_component_register(
    SRCS ${driver_srcs}
    INCLUDE_DIRS "include"
    PRIV_REQUIRES ${driver_requires} esp_lcd
)
