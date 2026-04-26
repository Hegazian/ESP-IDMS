#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

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
} idms_metrics_t;

void monitor_init(void);
void monitor_get_metrics(idms_metrics_t *out);
void monitor_adc_debug(int *out_mean, int *out_rms, int *out_errors);

#if CONFIG_IDMS_SCT_AUTOZERO_ENABLE
void monitor_calibrate_zero(void);
#endif
