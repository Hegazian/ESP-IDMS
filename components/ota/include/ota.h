#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

esp_err_t ota_init(void);

const char *ota_get_version(void);

const char *ota_get_status(void);

const char *ota_get_partition(void);

void ota_mark_app_valid(void);

void ota_mark_app_invalid_and_reboot(void);

bool ota_is_pending_validation(void);

void ota_schedule_valid_mark(uint32_t delay_ms);

bool ota_generate_token(char *out, size_t out_sz);

bool ota_check_token(const char *token);

uint32_t ota_get_max_app_size(void);
