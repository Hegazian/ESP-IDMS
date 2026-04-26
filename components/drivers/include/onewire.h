#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"

typedef struct {
    gpio_num_t pin;
    portMUX_TYPE mux;
    uint8_t rom_no[8];
    uint8_t last_discrepancy;
    uint8_t last_family_discrepancy;
    bool last_device_flag;
} ow_bus_t;

void ow_bus_init(ow_bus_t *bus, gpio_num_t pin);
bool ow_bus_reset(ow_bus_t *bus);
void ow_bus_write_byte(ow_bus_t *bus, uint8_t v);
uint8_t ow_bus_read_byte(ow_bus_t *bus);

void ow_bus_search_reset_state(ow_bus_t *bus);
bool ow_bus_search_next(ow_bus_t *bus, uint64_t *rom_code);

uint8_t ow_crc8(const uint8_t *addr, uint8_t len);
