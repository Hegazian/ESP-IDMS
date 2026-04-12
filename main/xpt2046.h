#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/spi_master.h"
#include "esp_err.h"

esp_err_t xpt2046_init(spi_host_device_t host);
bool xpt2046_read(int16_t *x, int16_t *y, bool *pressed);
