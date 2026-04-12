#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/gpio.h"

void ow_init(gpio_num_t pin);
bool ow_reset(void);
void ow_write_byte(uint8_t v);
uint8_t ow_read_byte(void);

void ow_search_reset_state(void);
bool ow_search_next(uint64_t *rom_code);

uint8_t ow_crc8(const uint8_t *addr, uint8_t len);
