#include "monitor.h"
#include "ds18b20.h"
#include "telegram.h"
#include "wifi_manager.h"
#include "sdkconfig.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "monitor";

static adc_oneshot_unit_handle_t s_adc;
static adc_channel_t s_adc_channel;
static portMUX_TYPE s_metrics_lock = portMUX_INITIALIZER_UNLOCKED;
static idms_metrics_t s_metrics;

static bool s_conv_pending_read;
static uint32_t s_heartbeat_ticks;

enum power_state_t { POWER_OK = 0, POWER_LOW_PENDING, POWER_FAULT };
enum cool_state_t { COOL_OK = 0, COOL_FAULT_LOW, COOL_FAULT_HIGH };

static enum power_state_t s_power_st = POWER_OK;
static enum cool_state_t s_cool_st = COOL_OK;
static uint32_t s_power_low_ms;
static uint32_t s_cool_bad_ms;

static float estimate_current_a_from_adc_rms(float v_rms)
{
    float scale = (float)CONFIG_IDMS_CT_AMPS_PER_VOLT_X100 / 100.0f;
    return v_rms * scale;
}

static float sample_current_rms_volts(void)
{
    const int n = 256;
    int32_t sum = 0;
    static int buf[256];
    for (int i = 0; i < n; i++) {
        int v = 0;
        if (adc_oneshot_read(s_adc, s_adc_channel, &v) != ESP_OK) {
            v = 0;
        }
        buf[i] = v;
        sum += v;
    }
    float mean = (float)sum / (float)n;
    double acc = 0.0;
    for (int i = 0; i < n; i++) {
        double d = (double)buf[i] - (double)mean;
        acc += d * d;
    }
    float rms_counts = (float)sqrt(acc / (double)n);
    return rms_counts * (3.3f / 4095.0f);
}

static void send_power_alert(bool loss, float a)
{
    if (loss) {
        char msg[96];
        snprintf(msg, sizeof(msg), "⚠️ ALERT: Machine power loss detected. Current: %.1fA", a);
        telegram_broadcast_text(msg);
    } else {
        telegram_broadcast_text("✅ Machine power has been restored.");
    }
}

static void send_cool_alert(bool low_side, float dt)
{
    if (low_side) {
        char msg[96];
        snprintf(msg, sizeof(msg), "⚠️ ALERT: Cooling failure. ΔT = %.1f°C (below minimum).", dt);
        telegram_broadcast_text(msg);
    } else {
        char msg[96];
        snprintf(msg, sizeof(msg), "⚠️ ALERT: Thermal overload. ΔT = %.1f°C (above maximum).", dt);
        telegram_broadcast_text(msg);
    }
}

static void send_cool_restored(void)
{
    telegram_broadcast_text("✅ Cooling system has returned to normal operation.");
}

static void monitor_task(void *arg)
{
    (void)arg;
    s_conv_pending_read = false;
    s_heartbeat_ticks = 0;

    const float i_thresh = (float)CONFIG_IDMS_CURRENT_THRESHOLD_MA / 1000.0f;
    const int dt_low = CONFIG_IDMS_DT_LOW_C;
    const int dt_high = CONFIG_IDMS_DT_HIGH_C;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(500));

        float vrms = sample_current_rms_volts();
        float amps = estimate_current_a_from_adc_rms(vrms);

        float t_in = 0.0f, t_out = 0.0f;
        bool v_in = false, v_out = false;
        if (s_conv_pending_read) {
            if (ds18b20_device_count() >= 1) {
                v_in = ds18b20_read_temperature_c(0, &t_in);
            }
            if (ds18b20_device_count() >= 2) {
                v_out = ds18b20_read_temperature_c(1, &t_out);
            }
        }
        ds18b20_request_conversion();
        s_conv_pending_read = true;

        float dt = 0.0f;
        bool v_dt = v_in && v_out;
        if (v_dt) {
            dt = t_out - t_in;
        }

        portENTER_CRITICAL(&s_metrics_lock);
        s_metrics.current_a = amps;
        s_metrics.current_valid = true;
        s_metrics.t_in_c = t_in;
        s_metrics.t_out_c = t_out;
        s_metrics.t_in_valid = v_in;
        s_metrics.t_out_valid = v_out;
        s_metrics.delta_t_c = dt;
        s_metrics.delta_valid = v_dt;
        s_metrics.wifi_connected = wifi_manager_is_connected();
        wifi_manager_get_ip(s_metrics.wifi_ip, sizeof(s_metrics.wifi_ip));
        portEXIT_CRITICAL(&s_metrics_lock);

        if (wifi_manager_is_connected()) {
            s_heartbeat_ticks++;
            if (s_heartbeat_ticks >= 120) {
                s_heartbeat_ticks = 0;
                telegram_heartbeat();
            }
        } else {
            s_heartbeat_ticks = 0;
        }

        if (!wifi_manager_is_connected()) {
            continue;
        }

        if (amps < i_thresh) {
            s_power_low_ms += 500;
        } else {
            if (s_power_st == POWER_FAULT) {
                send_power_alert(false, amps);
                s_power_st = POWER_OK;
            }
            s_power_low_ms = 0;
            s_power_st = POWER_OK;
        }

        if (amps < i_thresh && s_power_low_ms >= 5000 && s_power_st != POWER_FAULT) {
            send_power_alert(true, amps);
            s_power_st = POWER_FAULT;
        }

        if (v_dt) {
            bool bad_low = dt < (float)dt_low;
            bool bad_high = dt > (float)dt_high;
            bool bad = bad_low || bad_high;

            if (bad) {
                s_cool_bad_ms += 500;
            } else {
                if (s_cool_st == COOL_FAULT_LOW || s_cool_st == COOL_FAULT_HIGH) {
                    send_cool_restored();
                    s_cool_st = COOL_OK;
                }
                s_cool_bad_ms = 0;
                s_cool_st = COOL_OK;
            }

            if (bad && s_cool_bad_ms >= 5000) {
                if (bad_low && s_cool_st != COOL_FAULT_LOW) {
                    send_cool_alert(true, dt);
                    s_cool_st = COOL_FAULT_LOW;
                } else if (bad_high && s_cool_st != COOL_FAULT_HIGH) {
                    send_cool_alert(false, dt);
                    s_cool_st = COOL_FAULT_HIGH;
                }
            }
        }
    }
}

void monitor_init(void)
{
    const gpio_num_t adc_gpio = (gpio_num_t)CONFIG_IDMS_ADC_GPIO;
    adc_unit_t unit_id = ADC_UNIT_1;
    adc_channel_t ch = ADC_CHANNEL_0;
    esp_err_t err = adc_oneshot_io_to_channel(adc_gpio, &unit_id, &ch);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADC GPIO %d is not valid for this chip (%s). Fix IDMS_ADC_GPIO in menuconfig.",
                 (int)adc_gpio, esp_err_to_name(err));
        ESP_ERROR_CHECK(err);
    }
    s_adc_channel = ch;

    adc_oneshot_unit_init_cfg_t ucfg = {
        .unit_id = unit_id,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&ucfg, &s_adc));

    adc_oneshot_chan_cfg_t chcfg = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = ADC_ATTEN_DB_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, s_adc_channel, &chcfg));

    memset(&s_metrics, 0, sizeof(s_metrics));
    xTaskCreatePinnedToCore(monitor_task, "monitor", 8192, NULL, 5, NULL, tskNO_AFFINITY);
    ESP_LOGI(TAG, "Monitor task started (SCT-013 ADC on GPIO%d, adc unit %d channel %d)",
             (int)adc_gpio, (int)unit_id, (int)s_adc_channel);
}

void monitor_get_metrics(idms_metrics_t *out)
{
    if (!out) {
        return;
    }
    portENTER_CRITICAL(&s_metrics_lock);
    *out = s_metrics;
    portEXIT_CRITICAL(&s_metrics_lock);
}
