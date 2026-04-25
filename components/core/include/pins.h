#pragma once

#include "driver/spi_master.h"
#include "sdkconfig.h"

/* MAX31865 RTD sensors — dedicated SPI2 bus */
#define IDMS_SENSOR_SPI_HOST SPI2_HOST

/* LCD display — SPI3 bus (only when using SPI LCD) */
#if CONFIG_IDMS_LCD_BUS_SPI && !CONFIG_IDMS_DISPLAY_TOPWAY
#define IDMS_LCD_SPI_HOST SPI3_HOST
#endif

/* XPT2046 touch — SPI3 bus (dedicated when display is not SPI; shared when display is SPI) */
#if CONFIG_IDMS_PIN_TOUCH_CS >= 0
#if defined(IDMS_LCD_SPI_HOST)
/* Touch shares the LCD SPI bus — both on SPI3. This works if pins are distinct (CS, SCLK, MOSI, MISO). */
#define IDMS_TOUCH_SPI_HOST IDMS_LCD_SPI_HOST
#else
/* No SPI display — touch gets its own SPI3 bus */
#define IDMS_TOUCH_SPI_HOST SPI3_HOST
#endif
#endif
