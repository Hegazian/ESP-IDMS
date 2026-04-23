#pragma once

#include "driver/spi_master.h"
#include "sdkconfig.h"

#define IDMS_SENSOR_SPI_HOST SPI2_HOST

#if CONFIG_IDMS_LCD_BUS_SPI && !CONFIG_IDMS_DISPLAY_TOPWAY
#define IDMS_LCD_SPI_HOST SPI3_HOST
#endif
