#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

typedef struct {
    uint32_t samples;
    uint32_t current_valid_samples;
    uint32_t t_in_valid_samples;
    uint32_t t_out_valid_samples;
    uint32_t delta_valid_samples;
    float current_avg;
    float current_min;
    float current_max;
    float t_in_avg;
    float t_in_min;
    float t_in_max;
    float t_out_avg;
    float t_out_min;
    float t_out_max;
    float delta_avg;
    float delta_min;
    float delta_max;
    uint32_t power_fault_events;
    uint32_t cooling_fault_events;
    uint32_t sensor_fault_events;
    time_t since_utc;
    time_t last_sample_utc;
    bool time_synced;
} telemetry_weekly_stats_t;

esp_err_t telemetry_init(void);
void telemetry_get_weekly_stats(telemetry_weekly_stats_t *out);
void telemetry_build_weekly_report(char *buf, size_t buf_sz);
bool telemetry_weekly_report_due(void);
void telemetry_mark_weekly_report_sent(void);
const char *telemetry_csv_path(void);
