#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"

#define IDMS_SENSOR_ERR_CURRENT_ADC    (1u << 0)
#define IDMS_SENSOR_ERR_CURRENT_RANGE  (1u << 1)
#define IDMS_SENSOR_ERR_CURRENT_CAL    (1u << 2)
#define IDMS_SENSOR_ERR_TEMP_INIT      (1u << 3)
#define IDMS_SENSOR_ERR_TEMP_IN        (1u << 4)
#define IDMS_SENSOR_ERR_TEMP_OUT       (1u << 5)
#define IDMS_SENSOR_ERR_TEMP_DELTA     (1u << 6)

typedef struct {
    float current_a;
    bool current_valid;
    float t_in_c;
    float t_out_c;
    bool t_in_valid;
    bool t_out_valid;
    float delta_t_c;
    bool delta_valid;
    bool wifi_connected;
    char wifi_ip[16];
    time_t timestamp_utc;
    bool time_synced;
    bool power_fault;
    bool cooling_fault;
    bool delta_alert;
    bool sensor_preflight_done;
    bool sensor_preflight_ok;
    uint32_t sensor_error_flags;
    char sensor_status[96];
} idms_metrics_t;

esp_err_t monitor_init(void);
void monitor_get_metrics(idms_metrics_t *out);
void monitor_adc_debug(int *out_mean, int *out_rms, int *out_errors);

#if CONFIG_IDMS_SCT_AUTOZERO_ENABLE
esp_err_t monitor_calibrate_zero(void);
#endif
