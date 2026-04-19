#pragma once

#include "driver/spi_master.h"
#include "sdkconfig.h"

#if CONFIG_IDMS_LCD_HOST == 3
#define IDMS_SPI_HOST SPI3_HOST
#else
#define IDMS_SPI_HOST SPI2_HOST
#endif

#define IDMS_PIN_ONEWIRE CONFIG_IDMS_PIN_ONEWIRE
