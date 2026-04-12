#pragma once

#include <stdbool.h>
#include <stddef.h>

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
} idms_metrics_t;

void monitor_init(void);
void monitor_get_metrics(idms_metrics_t *out);
