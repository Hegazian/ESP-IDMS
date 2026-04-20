#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/spi_master.h"

/**
 * MAX31865 RTD-to-Digital converter driver for PT100/PT1000 sensors.
 *
 * Supports up to 2 sensors on the same SPI bus with separate CS pins.
 * Configuration is via Kconfig (IDMS_MAX31865_* settings).
 */

/**
 * Initialize the MAX31865 driver.
 * Configures SPI devices for both sensor channels, sets auto-conversion mode.
 * Must be called after the SPI bus is initialized (by the display init).
 *
 * @param spi_host  The SPI host (FSPI/SPI2/SPI3) the devices are connected to
 * @return ESP_OK on success
 */
esp_err_t max31865_init(spi_host_device_t spi_host);

/**
 * Read temperature from a sensor channel.
 *
 * @param index  Sensor index: 0 = T_in, 1 = T_out
 * @param out_c  Output temperature in degrees Celsius
 * @return       true if read succeeded, false on fault or error
 */
bool max31865_read_temperature_c(int index, float *out_c);

/**
 * Get the number of detected/initialized sensor channels.
 * Returns 0, 1, or 2 depending on Kconfig and init success.
 */
int max31865_sensor_count(void);