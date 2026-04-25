#include "monitor.h"
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
#include <time.h>

#if CONFIG_LWIP_SNTP_MAX_SERVERS > 0
#include "esp_sntp.h"
#endif

#if CONFIG_IDMS_TEMP_SENSOR_DS18B20 || CONFIG_IDMS_TEMP_SENSOR_DS18B20_PT100
#include "ds18b20.h"
#endif
#if CONFIG_IDMS_TEMP_SENSOR_MAX31865
#include "max31865.h"
#include "pins.h"
#endif
#if CONFIG_IDMS_TEMP_SENSOR_PT100_ADC || CONFIG_IDMS_TEMP_SENSOR_DS18B20_PT100
#include "adc_pt100.h"
#endif

#if CONFIG_IDMS_UI_ENABLE
#include "wifi_manager.h"
#include "telegram.h"
#endif

#include "config_store.h"

static const char *TAG = "monitor";

static adc_oneshot_unit_handle_t s_adc;
static adc_channel_t s_adc_channel;
static portMUX_TYPE s_metrics_lock = portMUX_INITIALIZER_UNLOCKED;
static idms_metrics_t s_metrics;

#if CONFIG_IDMS_TEMP_SENSOR_PT100_ADC || CONFIG_IDMS_TEMP_SENSOR_DS18B20_PT100
static pt100_adc_t s_pt100_in;
#if CONFIG_IDMS_PIN_PT100_ADC2 >= 0
static pt100_adc_t s_pt100_out;
#endif
#endif

static bool s_conv_pending_read;
static uint32_t s_heartbeat_ticks;

enum power_state_t { POWER_OK = 0, POWER_LOW_PENDING, POWER_FAULT };
enum cool_state_t { COOL_OK = 0, COOL_FAULT_LOW, COOL_FAULT_HIGH };

static enum power_state_t s_power_st = POWER_OK;
static enum cool_state_t s_cool_st = COOL_OK;
static uint32_t s_power_low_ms;
static uint32_t s_cool_bad_ms;

#if CONFIG_IDMS_UI_ENABLE
static bool s_power_alert_pending;
static bool s_power_restore_pending;
static bool s_cool_alert_pending;
static bool s_cool_alert_low_side;
static float s_cool_alert_pending_dt;
static bool s_cool_restore_pending;
#endif

#if CONFIG_IDMS_SCT_AUTOZERO_ENABLE
static float s_adc_offset_volts = 0.0f;
#endif

static float estimate_current_a_from_adc_rms(float v_rms)
{
    float scale = (float)CONFIG_IDMS_CT_AMPS_PER_VOLT_X100 / 100.0f;
    return v_rms * scale;
}

static float sample_current_rms_volts(void)
{
    const int n = 256;
    int32_t sum = 0;
    int64_t sum_sq = 0;
    
    /* Single-pass RMS calculation using integer math where possible */
    for (int i = 0; i < n; i++) {
        int v = 0;
        if (adc_oneshot_read(s_adc, s_adc_channel, &v) != ESP_OK) {
            v = 0;
        }
        sum += v;
        sum_sq += v * v;
    }
    
    /* Calculate RMS: sqrt((sum_sq/n) - (sum/n)^2) */
    float mean = (float)sum / (float)n;
    float mean_sq = (float)sum_sq / (float)n;
    float variance = mean_sq - (mean * mean);
    if (variance < 0.0f) variance = 0.0f;  /* Prevent negative due to floating point errors */
    float rms_counts = sqrtf(variance);
    float vrms = rms_counts * (3.3f / 4095.0f);

#if CONFIG_IDMS_SCT_AUTOZERO_ENABLE
    vrms -= s_adc_offset_volts;
    if (vrms < 0.0f) {
        vrms = 0.0f;
    }
#endif

    return vrms;
}

#if CONFIG_IDMS_SCT_AUTOZERO_ENABLE
void monitor_calibrate_zero(void)
{
    const int n = CONFIG_IDMS_SCT_AUTOZERO_SAMPLES;
    int32_t sum = 0;
    int64_t sum_sq = 0;

    ESP_LOGI(TAG, "Starting SCT-013 auto-zero calibration (%d samples)...", n);
    for (int i = 0; i < n; i++) {
        int v = 0;
        if (adc_oneshot_read(s_adc, s_adc_channel, &v) != ESP_OK) {
            v = 0;
        }
        sum += v;
        sum_sq += (int64_t)v * (int64_t)v;
        vTaskDelay(pdMS_TO_TICKS(1));  /* Small delay between samples */
    }

    float mean = (float)sum / (float)n;
    float mean_sq = (float)sum_sq / (float)n;
    float variance = mean_sq - (mean * mean);
    if (variance < 0.0f) {
        variance = 0.0f;
    }
    float rms_counts = sqrtf(variance);
    s_adc_offset_volts = rms_counts * (3.3f / 4095.0f);

    ESP_LOGI(TAG, "Auto-zero complete: offset = %.4f V (%.1f ADC counts)",
             s_adc_offset_volts, (double)rms_counts);
}
#endif

#if CONFIG_IDMS_UI_ENABLE
#define ALERT_MSG_BUF_SIZE 256

static void send_power_alert(bool loss, float a)
{
    if (loss) {
        char msg[ALERT_MSG_BUF_SIZE];
        int written = snprintf(msg, sizeof(msg), "\xe2\x9a\xa0\xef\xb8\x8f ALERT: Machine power loss detected. Current: %.2fA", (double)a);
        if (written > 0 && written < (int)sizeof(msg)) {
            uint8_t n = config_get_tech_count();
            for (int i = 0; i < n; i++) {
                char id[64];
                if (config_get_tech_id(i, id, sizeof(id)) == ESP_OK) {
                    telegram_send_ringing_alert(id, msg);
                }
            }
        }
    } else {
        telegram_broadcast_text("\xe2\x9c\x85 \xf0\x9f\x94\xb4 Machine power has been RESTORED. Alert cancelled.");
    }
}

static void send_cool_alert(bool low_side, float dt)
{
    char msg[ALERT_MSG_BUF_SIZE];
    int written;
    if (low_side) {
        written = snprintf(msg, sizeof(msg), "\xe2\x9a\xa0\xef\xb8\x8f ALERT: Cooling failure. \xe2\x96\xb3T = %.2f\xc2\xb0" "C (below minimum).", (double)dt);
    } else {
        written = snprintf(msg, sizeof(msg), "\xe2\x9a\xa0\xef\xb8\x8f ALERT: Thermal overload. \xe2\x96\xb3T = %.2f\xc2\xb0" "C (above maximum).", (double)dt);
    }
    if (written > 0 && written < (int)sizeof(msg)) {
        uint8_t n = config_get_tech_count();
        for (int i = 0; i < n; i++) {
            char id[64];
            if (config_get_tech_id(i, id, sizeof(id)) == ESP_OK) {
                telegram_send_ringing_alert(id, msg);
            }
        }
    }
}

static void send_cool_restored(void)
{
    telegram_broadcast_text("\xe2\x9c\x85 \xf0\x9f\x94\xb4 Cooling system has returned to NORMAL. Alert cancelled.");
}
#endif

static void monitor_task(void *arg)
{
    (void)arg;
    s_conv_pending_read = false;
    s_heartbeat_ticks = 0;

    const float i_thresh = (float)CONFIG_IDMS_CURRENT_THRESHOLD_MA / 1000.0f;
    const int dt_low = CONFIG_IDMS_DT_LOW_C;
    const int dt_high = CONFIG_IDMS_DT_HIGH_C;

#if CONFIG_IDMS_TEMP_SENSOR_MAX31865
    int sensor_count = max31865_sensor_count();
    ESP_LOGI(TAG, "MAX31865 sensors: %d", sensor_count);
#endif

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(500));

        float vrms = sample_current_rms_volts();
        float amps = estimate_current_a_from_adc_rms(vrms);

        float t_in = 0.0f, t_out = 0.0f;
        bool v_in = false, v_out = false;

#if CONFIG_IDMS_TEMP_SENSOR_DS18B20
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
#elif CONFIG_IDMS_TEMP_SENSOR_MAX31865
        v_in = max31865_read_temperature_c(0, &t_in);
        v_out = max31865_read_temperature_c(1, &t_out);
#elif CONFIG_IDMS_TEMP_SENSOR_PT100_ADC
        v_in = pt100_adc_read_celsius(&s_pt100_in, &t_in);
        v_out = false;
#elif CONFIG_IDMS_TEMP_SENSOR_DS18B20_PT100
        {
            bool ds18b20_read_done = s_conv_pending_read;
            float ds_t = 0.0f;
            bool ds_v = false;

            if (ds18b20_read_done && ds18b20_device_count() >= 1) {
                ds_v = ds18b20_read_temperature_c(0, &ds_t);
            }
            ds18b20_request_conversion();
            s_conv_pending_read = true;

    #if CONFIG_IDMS_COMBO_T_IN_DS18B20
            v_in = ds_v;
            t_in = ds_t;
    #elif CONFIG_IDMS_COMBO_T_IN_PT100
            v_in = pt100_adc_read_celsius(&s_pt100_in, &t_in);
    #endif

    #if CONFIG_IDMS_COMBO_T_OUT_DS18B20
            if (ds18b20_read_done && ds18b20_device_count() >= 2) {
                float ds_t2 = 0.0f;
                v_out = ds18b20_read_temperature_c(1, &ds_t2);
                t_out = ds_t2;
            }
    #elif CONFIG_IDMS_COMBO_T_OUT_PT100
        #if CONFIG_IDMS_PIN_PT100_ADC2 >= 0
            v_out = pt100_adc_read_celsius(&s_pt100_out, &t_out);
        #else
            v_out = pt100_adc_read_celsius(&s_pt100_in, &t_out);
        #endif
    #endif
        }
#endif

        float dt = 0.0f;
        bool v_dt = v_in && v_out;
        if (v_dt) {
            dt = t_out - t_in;
        }

#if CONFIG_IDMS_UI_ENABLE
        bool wifi_up = wifi_manager_is_connected();
#endif
#if CONFIG_LWIP_SNTP_MAX_SERVERS > 0
        bool time_synced = (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED);
        time_t timestamp_utc = time_synced ? time(NULL) : 0;
#endif

        portENTER_CRITICAL(&s_metrics_lock);
        s_metrics.current_a = amps;
        s_metrics.current_valid = true;
        s_metrics.t_in_c = t_in;
        s_metrics.t_out_c = t_out;
        s_metrics.t_in_valid = v_in;
        s_metrics.t_out_valid = v_out;
        s_metrics.delta_t_c = dt;
        s_metrics.delta_valid = v_dt;
#if CONFIG_IDMS_UI_ENABLE
        s_metrics.wifi_connected = wifi_up;
        wifi_manager_get_ip(s_metrics.wifi_ip, sizeof(s_metrics.wifi_ip));
#endif
#if CONFIG_LWIP_SNTP_MAX_SERVERS > 0
        s_metrics.time_synced = time_synced;
        s_metrics.timestamp_utc = timestamp_utc;
#endif
        portEXIT_CRITICAL(&s_metrics_lock);

        /* Power fault detection — ALWAYS runs, independent of Wi-Fi */
        if (amps < i_thresh) {
            s_power_low_ms += 500;
            if (s_power_low_ms >= 5000 && s_power_st == POWER_OK) {
                s_power_st = POWER_LOW_PENDING;
            }
            if (s_power_low_ms >= 5000 && s_power_st == POWER_LOW_PENDING) {
#if CONFIG_IDMS_UI_ENABLE
                if (wifi_up) {
                    send_power_alert(true, amps);
                } else {
                    s_power_alert_pending = true;
                }
#endif
                s_power_st = POWER_FAULT;
            }
        } else {
            if (s_power_st == POWER_FAULT) {
#if CONFIG_IDMS_UI_ENABLE
                if (wifi_up) {
                    send_power_alert(false, amps);
                } else {
                    s_power_restore_pending = true;
                }
#endif
            }
            s_power_low_ms = 0;
            s_power_st = POWER_OK;
        }

        /* Cooling fault detection — ALWAYS runs, independent of Wi-Fi */
        if (v_dt) {
            bool bad_low = dt < (float)dt_low;
            bool bad_high = dt > (float)dt_high;
            bool bad = bad_low || bad_high;

            if (bad) {
                s_cool_bad_ms += 500;
                if (s_cool_bad_ms >= 5000 && s_cool_st == COOL_OK) {
#if CONFIG_IDMS_UI_ENABLE
                    if (wifi_up) {
                        send_cool_alert(bad_low, dt);
                    } else {
                        s_cool_alert_pending = true;
                        s_cool_alert_low_side = bad_low;
                        s_cool_alert_pending_dt = dt;
                    }
#endif
                    if (bad_low) {
                        s_cool_st = COOL_FAULT_LOW;
                    } else {
                        s_cool_st = COOL_FAULT_HIGH;
                    }
                }
            } else {
                if (s_cool_st == COOL_FAULT_LOW || s_cool_st == COOL_FAULT_HIGH) {
#if CONFIG_IDMS_UI_ENABLE
                    if (wifi_up) {
                        send_cool_restored();
                    } else {
                        s_cool_restore_pending = true;
                    }
#endif
                }
                s_cool_bad_ms = 0;
                s_cool_st = COOL_OK;
            }
        } else {
            if (s_cool_st == COOL_FAULT_LOW || s_cool_st == COOL_FAULT_HIGH) {
#if CONFIG_IDMS_UI_ENABLE
                ESP_LOGW(TAG, "Temperature sensor data invalid, resetting cooling fault state");
#endif
            }
            s_cool_bad_ms = 0;
            s_cool_st = COOL_OK;
        }

#if CONFIG_IDMS_UI_ENABLE
        /* Wi-Fi dependent operations: heartbeat and flush pending alerts */
        if (wifi_up) {
            s_heartbeat_ticks++;
            if (s_heartbeat_ticks >= 120) {
                s_heartbeat_ticks = 0;
                telegram_heartbeat();
            }

            if (s_power_alert_pending) {
                send_power_alert(true, amps);
                s_power_alert_pending = false;
            }
            if (s_power_restore_pending) {
                send_power_alert(false, amps);
                s_power_restore_pending = false;
            }
            if (s_cool_alert_pending) {
                send_cool_alert(s_cool_alert_low_side, s_cool_alert_pending_dt);
                s_cool_alert_pending = false;
            }
            if (s_cool_restore_pending) {
                send_cool_restored();
                s_cool_restore_pending = false;
            }
        } else {
            s_heartbeat_ticks = 0;
        }
#endif
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

#if CONFIG_IDMS_SCT_AUTOZERO_ENABLE
    monitor_calibrate_zero();
#endif

#if CONFIG_IDMS_TEMP_SENSOR_DS18B20
    ds18b20_init();
    ESP_LOGI(TAG, "Temperature sensors: DS18B20 1-Wire (T_in=GPIO%d, T_out=GPIO%d)",
             CONFIG_IDMS_PIN_ONEWIRE, CONFIG_IDMS_PIN_ONEWIRE2);
#elif CONFIG_IDMS_TEMP_SENSOR_MAX31865
    max31865_init(IDMS_SENSOR_SPI_HOST);
    ESP_LOGI(TAG, "Temperature sensors: PT100 via MAX31865 (CS0=GPIO%d, CS1=GPIO%d)",
             CONFIG_IDMS_MAX31865_CS0, CONFIG_IDMS_MAX31865_CS1);
#elif CONFIG_IDMS_TEMP_SENSOR_PT100_ADC
    pt100_adc_init(&s_pt100_in, CONFIG_IDMS_PIN_PT100_ADC,
                   (float)CONFIG_IDMS_PT100_RREF_X10 / 10.0f,
                   (float)CONFIG_IDMS_PT100_R0_X10 / 10.0f);
    ESP_LOGI(TAG, "Temperature sensors: PT100 via ADC (GPIO%d, R_ref=%.1fΩ)",
             CONFIG_IDMS_PIN_PT100_ADC, (float)CONFIG_IDMS_PT100_RREF_X10 / 10.0f);
#elif CONFIG_IDMS_TEMP_SENSOR_DS18B20_PT100
    ds18b20_init();
    pt100_adc_init(&s_pt100_in, CONFIG_IDMS_PIN_PT100_ADC,
                   (float)CONFIG_IDMS_PT100_RREF_X10 / 10.0f,
                   (float)CONFIG_IDMS_PT100_R0_X10 / 10.0f);
#if CONFIG_IDMS_PIN_PT100_ADC2 >= 0
    pt100_adc_init(&s_pt100_out, CONFIG_IDMS_PIN_PT100_ADC2,
                   (float)CONFIG_IDMS_PT100_RREF_X10 / 10.0f,
                   (float)CONFIG_IDMS_PT100_R0_X10 / 10.0f);
#endif
    ESP_LOGI(TAG, "Temperature sensors: DS18B20+PT100 combo (1-Wire=GPIO%d/%d, PT100 ADC=GPIO%d)",
             CONFIG_IDMS_PIN_ONEWIRE, CONFIG_IDMS_PIN_ONEWIRE2, CONFIG_IDMS_PIN_PT100_ADC);
#endif

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