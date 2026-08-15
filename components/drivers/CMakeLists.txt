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

list(APPEND driver_srcs "topway_lcd.c")

idf_component_register(
    SRCS ${driver_srcs}
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_uart
    PRIV_REQUIRES ${driver_requires}
)
