#include "adc_pt100.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include <math.h>

static const char *TAG = "adc_pt100";

#define RTD_A 3.9083e-3f
#define RTD_B -5.775e-7f

esp_err_t pt100_adc_init(pt100_adc_t *ctx, adc_oneshot_unit_handle_t adc, int gpio, float r_ref, float r0)
{
    if (!ctx) return ESP_ERR_INVALID_ARG;

    adc_unit_t unit_id = ADC_UNIT_1;
    adc_channel_t ch = ADC_CHANNEL_0;
    esp_err_t err = adc_oneshot_io_to_channel(gpio, &unit_id, &ch);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO %d is not a valid ADC channel: %s", gpio, esp_err_to_name(err));
        return err;
    }

    adc_oneshot_chan_cfg_t chcfg = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = ADC_ATTEN_DB_12,
    };
    err = adc_oneshot_config_channel(adc, ch, &chcfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADC channel config failed for GPIO %d: %s", gpio, esp_err_to_name(err));
        return err;
    }

    ctx->adc = adc;
    ctx->channel = ch;
    ctx->r_ref = r_ref;
    ctx->r0 = r0;

    ESP_LOGI(TAG, "PT100 ADC initialized (GPIO=%d, R_ref=%.1fΩ, R0=%.1fΩ)", gpio, r_ref, r0);
    return ESP_OK;
}

bool pt100_adc_read_celsius(pt100_adc_t *ctx, float *out_c)
{
    if (!ctx || !out_c) return false;

    const int n = 16;
    int32_t sum = 0;
    for (int i = 0; i < n; i++) {
        int raw = 0;
        if (adc_oneshot_read(ctx->adc, ctx->channel, &raw) != ESP_OK) {
            raw = 0;
        }
        sum += raw;
    }
    int avg_raw = (int)(sum / n);

    float v_out = (float)avg_raw * 3.3f / 4095.0f;

    if (v_out < 0.05f || v_out > 3.25f) {
        return false;
    }

    float r_pt100 = ctx->r_ref * v_out / (3.3f - v_out);

    if (r_pt100 < 10.0f || r_pt100 > 500.0f) {
        return false;
    }

    float z = r_pt100 / ctx->r0;
    float discriminant = RTD_A * RTD_A - 4.0f * RTD_B * (1.0f - z);
    float temp;
    if (discriminant < 0) {
        temp = (r_pt100 - ctx->r0) / (ctx->r0 * RTD_A);
    } else {
        temp = (-RTD_A + sqrtf(discriminant)) / (2.0f * RTD_B);
    }

    if (temp < -100.0f || temp > 400.0f) {
        return false;
    }

    *out_c = temp;
    return true;
}
