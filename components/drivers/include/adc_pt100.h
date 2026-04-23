#pragma once

#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    adc_oneshot_unit_handle_t adc;
    adc_channel_t channel;
    float r_ref;
    float r0;
} pt100_adc_t;

esp_err_t pt100_adc_init(pt100_adc_t *ctx, adc_oneshot_unit_handle_t adc, int gpio, float r_ref, float r0);

bool pt100_adc_read_celsius(pt100_adc_t *ctx, float *out_c);

#ifdef __cplusplus
}
#endif
