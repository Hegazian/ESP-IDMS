#pragma once

#include <stdbool.h>
#include <stdint.h>

void ds18b20_init(void);
void ds18b20_request_conversion(void);
bool ds18b20_read_temperature_c(int index, float *out_c);

int ds18b20_device_count(void);
bool ds18b20_sensor_present(int index);
