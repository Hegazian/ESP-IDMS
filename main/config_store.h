#pragma once

#include <stddef.h>
#include "esp_err.h"

esp_err_t config_store_init(void);

uint8_t config_get_tech_count(void);
esp_err_t config_get_tech_id(int idx, char *out, size_t out_len);
esp_err_t config_set_tech_id(int idx, const char *id);
esp_err_t config_add_tech_id(const char *id);
