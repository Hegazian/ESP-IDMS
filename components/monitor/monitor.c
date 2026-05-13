#include "monitor.h"
#include "sdkconfig.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
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
static adc_cali_handle_t s_adc_cali;
static bool s_adc_cali_enabled;
static uint32_t s_sensor_error_flags;
static uint32_t s_sensor_init_error_flags;
static uint32_t s_sensor_last_logged_flags = UINT32_MAX;
static char s_sensor_status[96] = "Sensor status pending";
static portMUX_TYPE s_metrics_lock = portMUX_INITIALIZER_UNLOCKED;
static idms_metrics_t s_metrics;

#if CONFIG_IDMS_TEMP_SENSOR_PT100_ADC || CONFIG_IDMS_TEMP_SENSOR_DS18B20_PT100
static pt100_adc_t s_pt100_in;
#if CONFIG_IDMS_PIN_PT100_ADC2 >= 0
static pt100_adc_t s_pt100_out;
#endif
#endif

static bool s_conv_pending_read;
static uint32_t s_ds18b20_request_ms;
static uint32_t s_ds18b20_last_rescan_ms;
static float s_last_t_in_c;
static float s_last_t_out_c;
static bool s_last_t_in_valid;
static bool s_last_t_out_valid;
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
static bool s_sensor_preflight_alert_pending;
#endif

#if CONFIG_IDMS_SCT_AUTOZERO_ENABLE
static float s_adc_offset_volts = 0.0f;
#endif

static float estimate_current_a_from_adc_rms(float v_rms)
{
    float scale = (float)config_get_current_cal_x100() / 100.0f;
    return v_rms * scale;
}

typedef struct {
    float mean_raw;
    float rms_raw;
    float mean_mv;
    float rms_mv;
    int read_errors;
    int samples;
    bool connected;
} current_sample_stats_t;

static int s_adc_err_count;
static bool s_adc_sensor_connected = false;
static bool s_current_zero_valid = false;
static float s_current_ema = 0.0f;
static bool s_current_ema_initialized = false;
static uint8_t s_current_recovery_good_count;
#define CURRENT_EMA_ALPHA 0.15f
#define MONITOR_PERIOD_MS 500
#define CURRENT_ADC_BIAS_MIN_RAW 900.0f
#define CURRENT_ADC_BIAS_MAX_RAW 3200.0f
#define CURRENT_ADC_WARMUP_SAMPLES 32
#define CURRENT_RECOVERY_REQUIRED_GOOD_READS 3
#define CURRENT_AUTOZERO_MAX_RMS_MV ((float)CONFIG_IDMS_SCT_AUTOZERO_MAX_RMS_MV)
#define DS18B20_CONVERSION_MS 800
#define DS18B20_RESCAN_MS 10000

static bool current_adc_bias_ok(float mean_raw)
{
    return mean_raw >= CURRENT_ADC_BIAS_MIN_RAW && mean_raw <= CURRENT_ADC_BIAS_MAX_RAW;
}

static uint32_t preflight_current_sensor(void);
static uint32_t preflight_temperature_sensors(void);
static void set_sensor_status_flags(uint32_t flags);

static int current_adc_raw_to_mv(int raw)
{
    int voltage_mv = (raw * 3300) / 4095;
    if (s_adc_cali_enabled && s_adc_cali) {
        if (adc_cali_raw_to_voltage(s_adc_cali, raw, &voltage_mv) != ESP_OK) {
            voltage_mv = (raw * 3300) / 4095;
        }
    }
    return voltage_mv;
}

static esp_err_t sample_current_stats(int n, TickType_t delay_ticks, current_sample_stats_t *out)
{
    if (!out || n <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    if (!s_adc) {
        out->read_errors = n;
        return ESP_ERR_INVALID_STATE;
    }

    int32_t sum = 0;
    int64_t sum_sq = 0;
    int32_t sum_mv = 0;
    int64_t sum_sq_mv = 0;
    int err_count = 0;
    int sample_count = 0;
    
    for (int i = 0; i < n; i++) {
        int v = 0;
        esp_err_t ret = adc_oneshot_read(s_adc, s_adc_channel, &v);
        if (ret != ESP_OK) {
            err_count++;
            if (delay_ticks > 0) {
                vTaskDelay(delay_ticks);
            }
            continue;
        }
        sum += v;
        sum_sq += (int64_t)v * (int64_t)v;
        int mv = current_adc_raw_to_mv(v);
        sum_mv += mv;
        sum_sq_mv += (int64_t)mv * (int64_t)mv;
        sample_count++;
        if (delay_ticks > 0) {
            vTaskDelay(delay_ticks);
        }
    }

    out->read_errors = err_count;
    out->samples = sample_count;
    if (sample_count == 0) {
        return ESP_FAIL;
    }
    
    out->mean_raw = (float)sum / (float)sample_count;
    float mean_sq = (float)sum_sq / (float)sample_count;
    float variance = mean_sq - (out->mean_raw * out->mean_raw);
    if (variance < 0.0f) {
        variance = 0.0f;
    }
    out->rms_raw = sqrtf(variance);

    out->mean_mv = (float)sum_mv / (float)sample_count;
    float mean_sq_mv = (float)sum_sq_mv / (float)sample_count;
    float variance_mv = mean_sq_mv - (out->mean_mv * out->mean_mv);
    if (variance_mv < 0.0f) {
        variance_mv = 0.0f;
    }
    out->rms_mv = sqrtf(variance_mv);
    out->connected = current_adc_bias_ok(out->mean_raw);

    return ESP_OK;
}

static float sample_current_rms_volts(void)
{
    current_sample_stats_t stats;
    esp_err_t err = sample_current_stats(512, 0, &stats);
    if (err != ESP_OK) {
        s_adc_err_count = stats.read_errors;
        s_adc_sensor_connected = false;
        return 0.0f;
    }

    s_adc_err_count = stats.read_errors;
    bool zero_ok = true;
#if CONFIG_IDMS_SCT_AUTOZERO_ENABLE
    zero_ok = s_current_zero_valid;
#endif
    s_adc_sensor_connected = stats.connected && stats.read_errors == 0 && zero_ok;
    float vrms = stats.rms_mv / 1000.0f;

#if CONFIG_IDMS_SCT_AUTOZERO_ENABLE
    vrms -= s_adc_offset_volts;
    if (vrms < 0.0f) {
        vrms = 0.0f;
    }
#endif

    return vrms;
}

#if CONFIG_IDMS_SCT_AUTOZERO_ENABLE
esp_err_t monitor_calibrate_zero(void)
{
    const int n = CONFIG_IDMS_SCT_AUTOZERO_SAMPLES;
    current_sample_stats_t stats;

    ESP_LOGI(TAG, "Starting SCT-013 auto-zero calibration (%d samples)...", n);
    esp_err_t err = sample_current_stats(n, pdMS_TO_TICKS(1), &stats);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Auto-zero failed: ADC sampling error (%s)", esp_err_to_name(err));
        return err;
    }
    if (stats.read_errors > 0) {
        ESP_LOGE(TAG, "Auto-zero: %d/%d ADC reads FAILED (GPIO%d may not support ADC)",
                 stats.read_errors, n, CONFIG_IDMS_ADC_GPIO);
        return ESP_FAIL;
    }
    if (!stats.connected) {
        s_current_zero_valid = false;
        ESP_LOGE(TAG, "Auto-zero failed: ADC bias mean %.1f outside expected %.0f-%.0f counts",
                 (double)stats.mean_raw,
                 (double)CURRENT_ADC_BIAS_MIN_RAW,
                 (double)CURRENT_ADC_BIAS_MAX_RAW);
        ESP_LOGE(TAG, "Check TP_ADC: expected about 1.65V with 2x10k bias divider");
        return ESP_ERR_INVALID_STATE;
    }
    if (stats.rms_mv > CURRENT_AUTOZERO_MAX_RMS_MV) {
        s_adc_offset_volts = 0.0f;
        s_current_zero_valid = true;
        ESP_LOGW(TAG,
                 "Auto-zero skipped: RMS %.2f mV is above no-load limit %.2f mV. "
                 "Load may be running; current readings will use raw calibrated RMS.",
                 (double)stats.rms_mv, (double)CURRENT_AUTOZERO_MAX_RMS_MV);
        ESP_LOGW(TAG, "Run 'cal_zero' only with the CT connected and the load OFF.");
        return ESP_OK;
    }

    s_adc_offset_volts = stats.rms_mv / 1000.0f;
    s_current_zero_valid = true;

    ESP_LOGI(TAG, "Auto-zero complete: offset = %.4f V (%.1f ADC counts), mean = %.1f",
             s_adc_offset_volts, (double)stats.rms_raw, (double)stats.mean_raw);
    return ESP_OK;
}
#endif

esp_err_t monitor_calibrate_all(void)
{
    uint32_t flags = 0;

    if (!s_adc) {
        flags |= IDMS_SENSOR_ERR_CURRENT_ADC;
    } else {
#if CONFIG_IDMS_SCT_AUTOZERO_ENABLE
        esp_err_t zero_err = monitor_calibrate_zero();
        if (zero_err != ESP_OK) {
            if (zero_err == ESP_ERR_INVALID_STATE) {
                flags |= IDMS_SENSOR_ERR_CURRENT_RANGE;
            } else {
                flags |= IDMS_SENSOR_ERR_CURRENT_CAL;
            }
        }
#else
        s_current_zero_valid = true;
#endif
        flags |= preflight_current_sensor();
    }

#if CONFIG_IDMS_TEMP_SENSOR_DS18B20
    ds18b20_init();
#elif CONFIG_IDMS_TEMP_SENSOR_DS18B20_PT100
    ds18b20_init();
#endif
    flags |= preflight_temperature_sensors();

    monitor_reset_current_filter();
    set_sensor_status_flags(flags);
    return flags == 0 ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static void append_status_item(char *buf, size_t buf_sz, const char *item)
{
    if (!buf || buf_sz == 0 || !item) {
        return;
    }
    size_t len = strlen(buf);
    if (len > 0 && len < buf_sz - 1) {
        int written = snprintf(buf + len, buf_sz - len, ", %s", item);
        (void)written;
    } else if (len == 0) {
        snprintf(buf, buf_sz, "%s", item);
    }
}

static void set_sensor_status_flags(uint32_t flags)
{
    char failed[80] = "";

    if (flags & IDMS_SENSOR_ERR_CURRENT_ADC) {
        append_status_item(failed, sizeof(failed), "Current ADC");
    }
    if (flags & IDMS_SENSOR_ERR_CURRENT_RANGE) {
        append_status_item(failed, sizeof(failed), "Current bias");
    }
    if (flags & IDMS_SENSOR_ERR_CURRENT_CAL) {
        append_status_item(failed, sizeof(failed), "Current cal");
    }
    if (flags & IDMS_SENSOR_ERR_TEMP_INIT) {
        append_status_item(failed, sizeof(failed), "Temp init");
    }
    if (flags & IDMS_SENSOR_ERR_TEMP_IN) {
        append_status_item(failed, sizeof(failed), "T_in");
    }
    if (flags & IDMS_SENSOR_ERR_TEMP_OUT) {
        append_status_item(failed, sizeof(failed), "T_out");
    }
    if (flags & IDMS_SENSOR_ERR_TEMP_DELTA) {
        append_status_item(failed, sizeof(failed), "Delta T");
    }

    s_sensor_error_flags = flags;
    if (flags == 0) {
        snprintf(s_sensor_status, sizeof(s_sensor_status), "Sensors OK");
    } else {
        snprintf(s_sensor_status, sizeof(s_sensor_status), "Sensor fault: %s", failed[0] ? failed : "failed");
    }

    if (flags != s_sensor_last_logged_flags) {
        if (flags == 0) {
            ESP_LOGI(TAG, "Sensor faults recovered");
        } else {
            ESP_LOGW(TAG, "Sensor status: %s (flags=0x%02lx)",
                     s_sensor_status, (unsigned long)flags);
        }
        s_sensor_last_logged_flags = flags;
    }

    portENTER_CRITICAL(&s_metrics_lock);
    s_metrics.sensor_preflight_done = true;
    s_metrics.sensor_preflight_ok = (flags == 0);
    s_metrics.sensor_error_flags = flags;
    strncpy(s_metrics.sensor_status, s_sensor_status, sizeof(s_metrics.sensor_status) - 1);
    s_metrics.sensor_status[sizeof(s_metrics.sensor_status) - 1] = '\0';
    portEXIT_CRITICAL(&s_metrics_lock);

#if CONFIG_IDMS_UI_ENABLE
    if (flags == 0) {
        s_sensor_preflight_alert_pending = false;
    }
#endif
}

static void mark_sensor_preflight_pending(void)
{
    portENTER_CRITICAL(&s_metrics_lock);
    s_metrics.sensor_preflight_done = false;
    s_metrics.sensor_preflight_ok = false;
    s_metrics.sensor_error_flags = 0;
    strncpy(s_metrics.sensor_status, "Sensor status pending", sizeof(s_metrics.sensor_status) - 1);
    s_metrics.sensor_status[sizeof(s_metrics.sensor_status) - 1] = '\0';
    portEXIT_CRITICAL(&s_metrics_lock);
}

static bool temp_in_range(float temp_c, float min_c, float max_c)
{
    return isfinite(temp_c) && temp_c >= min_c && temp_c <= max_c;
}

static bool init_current_adc_calibration(adc_unit_t unit_id, adc_channel_t channel,
                                         adc_atten_t atten, adc_bitwidth_t bitwidth)
{
    s_adc_cali = NULL;
    s_adc_cali_enabled = false;

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t curve_cfg = {
        .unit_id = unit_id,
        .chan = channel,
        .atten = atten,
        .bitwidth = bitwidth,
    };
    if (adc_cali_create_scheme_curve_fitting(&curve_cfg, &s_adc_cali) == ESP_OK) {
        s_adc_cali_enabled = true;
        ESP_LOGI(TAG, "Current ADC calibration: curve fitting");
        return true;
    }
#endif

#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t line_cfg = {
        .unit_id = unit_id,
        .atten = atten,
        .bitwidth = bitwidth,
    };
    if (adc_cali_create_scheme_line_fitting(&line_cfg, &s_adc_cali) == ESP_OK) {
        s_adc_cali_enabled = true;
        ESP_LOGI(TAG, "Current ADC calibration: line fitting");
        return true;
    }
#endif

    ESP_LOGW(TAG, "Current ADC calibration unavailable; using raw 3.3V conversion");
    return false;
}

static uint32_t preflight_current_sensor(void)
{
    current_sample_stats_t stats;
    uint32_t flags = 0;
    esp_err_t err = sample_current_stats(128, pdMS_TO_TICKS(1), &stats);

    if (err != ESP_OK || stats.read_errors > 0) {
        ESP_LOGE(TAG, "Current sensor preflight failed: ADC sampling err=%s read_errors=%d",
                 esp_err_to_name(err), stats.read_errors);
        flags |= IDMS_SENSOR_ERR_CURRENT_ADC;
    }
    if (err == ESP_OK && !stats.connected) {
        ESP_LOGE(TAG, "Current sensor preflight failed: ADC bias mean %.1f outside expected %.0f-%.0f counts",
                 (double)stats.mean_raw,
                 (double)CURRENT_ADC_BIAS_MIN_RAW,
                 (double)CURRENT_ADC_BIAS_MAX_RAW);
        ESP_LOGE(TAG, "Measure TP_ADC/GPIO%d: expected about 1.65V before current can be calibrated",
                 CONFIG_IDMS_ADC_GPIO);
        flags |= IDMS_SENSOR_ERR_CURRENT_RANGE;
    }
    if (err == ESP_OK && stats.connected && stats.read_errors == 0) {
        ESP_LOGI(TAG, "Current sensor preflight OK: mean=%.1f raw rms=%.1f mv rms=%.2f",
                 (double)stats.mean_raw, (double)stats.rms_raw, (double)stats.rms_mv);
    }

    return flags;
}

static void recover_current_sensor_if_ready(void)
{
    if (!s_adc) {
        s_current_recovery_good_count = 0;
        s_adc_sensor_connected = false;
        s_current_zero_valid = false;
        return;
    }

    if (s_adc_sensor_connected && s_current_zero_valid) {
        return;
    }

    current_sample_stats_t stats;
    esp_err_t err = sample_current_stats(64, 0, &stats);
    if (err != ESP_OK || stats.read_errors > 0 || !stats.connected) {
        s_current_recovery_good_count = 0;
        s_adc_sensor_connected = false;
        if (err == ESP_OK && !stats.connected) {
            s_current_zero_valid = false;
        }
        return;
    }

    if (s_current_recovery_good_count < CURRENT_RECOVERY_REQUIRED_GOOD_READS) {
        s_current_recovery_good_count++;
    }

    if (s_current_recovery_good_count < CURRENT_RECOVERY_REQUIRED_GOOD_READS) {
        return;
    }

#if CONFIG_IDMS_SCT_AUTOZERO_ENABLE
    ESP_LOGI(TAG, "Current ADC bias recovered: mean=%.1f raw rms=%.1f. Re-running zero calibration.",
             (double)stats.mean_raw, (double)stats.rms_raw);
    if (monitor_calibrate_zero() != ESP_OK) {
        s_adc_sensor_connected = false;
        s_current_zero_valid = false;
        s_current_recovery_good_count = 0;
        return;
    }
#else
    s_current_zero_valid = true;
#endif

    s_adc_sensor_connected = true;
    s_current_ema = 0.0f;
    s_current_ema_initialized = false;
    s_current_recovery_good_count = 0;
}

static uint32_t preflight_temperature_sensors(void)
{
    uint32_t flags = 0;

#if CONFIG_IDMS_TEMP_SENSOR_DS18B20
    bool tin_present = ds18b20_sensor_present(0);
    bool tout_present = ds18b20_sensor_present(1);
    if (!tin_present) {
        flags |= IDMS_SENSOR_ERR_TEMP_IN;
    }
    if (!tout_present) {
        flags |= IDMS_SENSOR_ERR_TEMP_OUT;
    }
    if (!tin_present || !tout_present) {
        flags |= IDMS_SENSOR_ERR_TEMP_DELTA;
    }
    if (tin_present || tout_present) {
        ds18b20_request_conversion();
        vTaskDelay(pdMS_TO_TICKS(800));
        float t_in = 0.0f;
        if (!tin_present ||
            !ds18b20_read_temperature_c(0, &t_in) ||
            !temp_in_range(t_in, -55.0f, 125.0f)) {
            flags |= IDMS_SENSOR_ERR_TEMP_IN | IDMS_SENSOR_ERR_TEMP_DELTA;
            s_last_t_in_valid = false;
        } else {
            s_last_t_in_c = t_in;
            s_last_t_in_valid = true;
        }
        float t_out = 0.0f;
        if (!tout_present ||
            !ds18b20_read_temperature_c(1, &t_out) ||
            !temp_in_range(t_out, -55.0f, 125.0f)) {
            flags |= IDMS_SENSOR_ERR_TEMP_OUT | IDMS_SENSOR_ERR_TEMP_DELTA;
            s_last_t_out_valid = false;
        } else {
            s_last_t_out_c = t_out;
            s_last_t_out_valid = true;
        }
    }
#elif CONFIG_IDMS_TEMP_SENSOR_MAX31865
    int count = max31865_sensor_count();
    if (count < 1) {
        flags |= IDMS_SENSOR_ERR_TEMP_IN;
    }
    if (count < 2) {
        flags |= IDMS_SENSOR_ERR_TEMP_OUT | IDMS_SENSOR_ERR_TEMP_DELTA;
    }
    float t = 0.0f;
    if (count >= 1 && (!max31865_read_temperature_c(0, &t) || !temp_in_range(t, -100.0f, 400.0f))) {
        flags |= IDMS_SENSOR_ERR_TEMP_IN;
    }
    if (count >= 2 && (!max31865_read_temperature_c(1, &t) || !temp_in_range(t, -100.0f, 400.0f))) {
        flags |= IDMS_SENSOR_ERR_TEMP_OUT | IDMS_SENSOR_ERR_TEMP_DELTA;
    }
#elif CONFIG_IDMS_TEMP_SENSOR_PT100_ADC
    float t = 0.0f;
    if (!pt100_adc_read_celsius(&s_pt100_in, &t) || !temp_in_range(t, -100.0f, 400.0f)) {
        flags |= IDMS_SENSOR_ERR_TEMP_IN;
    }
#elif CONFIG_IDMS_TEMP_SENSOR_DS18B20_PT100
    bool ds_requested = false;
    int count = ds18b20_device_count();
#if CONFIG_IDMS_COMBO_T_IN_DS18B20 || CONFIG_IDMS_COMBO_T_OUT_DS18B20
    if (count > 0) {
        ds18b20_request_conversion();
        ds_requested = true;
    }
    if (ds_requested) {
        vTaskDelay(pdMS_TO_TICKS(800));
    }
#endif
#if CONFIG_IDMS_COMBO_T_IN_DS18B20
    float t_in_ds = 0.0f;
    if (count < 1 || !ds18b20_read_temperature_c(0, &t_in_ds) ||
        !temp_in_range(t_in_ds, -55.0f, 125.0f)) {
        flags |= IDMS_SENSOR_ERR_TEMP_IN;
    }
#elif CONFIG_IDMS_COMBO_T_IN_PT100
    float t_in_pt = 0.0f;
    if (!pt100_adc_read_celsius(&s_pt100_in, &t_in_pt) || !temp_in_range(t_in_pt, -100.0f, 400.0f)) {
        flags |= IDMS_SENSOR_ERR_TEMP_IN;
    }
#endif
#if CONFIG_IDMS_COMBO_T_OUT_DS18B20
    float t_out_ds = 0.0f;
    if (count < 2 || !ds18b20_read_temperature_c(1, &t_out_ds) ||
        !temp_in_range(t_out_ds, -55.0f, 125.0f)) {
        flags |= IDMS_SENSOR_ERR_TEMP_OUT | IDMS_SENSOR_ERR_TEMP_DELTA;
    }
#elif CONFIG_IDMS_COMBO_T_OUT_PT100
    float t_out_pt = 0.0f;
#if CONFIG_IDMS_PIN_PT100_ADC2 >= 0
    if (!pt100_adc_read_celsius(&s_pt100_out, &t_out_pt) || !temp_in_range(t_out_pt, -100.0f, 400.0f)) {
#else
    if (!pt100_adc_read_celsius(&s_pt100_in, &t_out_pt) || !temp_in_range(t_out_pt, -100.0f, 400.0f)) {
#endif
        flags |= IDMS_SENSOR_ERR_TEMP_OUT | IDMS_SENSOR_ERR_TEMP_DELTA;
    }
#endif
#endif

    if (flags & (IDMS_SENSOR_ERR_TEMP_IN | IDMS_SENSOR_ERR_TEMP_OUT | IDMS_SENSOR_ERR_TEMP_DELTA)) {
        ESP_LOGE(TAG, "Temperature sensor preflight failed (flags=0x%02lx)",
                 (unsigned long)flags);
    } else {
        ESP_LOGI(TAG, "Temperature sensor preflight OK");
    }
    return flags;
}

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
    s_ds18b20_request_ms = 0;
    s_heartbeat_ticks = 0;

#if CONFIG_IDMS_TEMP_SENSOR_MAX31865
    int sensor_count = max31865_sensor_count();
    ESP_LOGI(TAG, "MAX31865 sensors: %d", sensor_count);
#endif

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(MONITOR_PERIOD_MS));
        uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

        recover_current_sensor_if_ready();
        float vrms = sample_current_rms_volts();
        float amps_raw = estimate_current_a_from_adc_rms(vrms);
        float amps;
        if (!s_current_ema_initialized) {
            s_current_ema = amps_raw;
            s_current_ema_initialized = true;
        } else {
            s_current_ema = CURRENT_EMA_ALPHA * amps_raw + (1.0f - CURRENT_EMA_ALPHA) * s_current_ema;
        }
        amps = s_current_ema;

        float t_in = 0.0f, t_out = 0.0f;
        bool v_in = false, v_out = false;

#if CONFIG_IDMS_TEMP_SENSOR_DS18B20
        if ((!ds18b20_sensor_present(0) || !ds18b20_sensor_present(1)) &&
            (now_ms - s_ds18b20_last_rescan_ms) >= DS18B20_RESCAN_MS) {
            ESP_LOGI(TAG, "Re-scanning DS18B20 buses for recovered sensors");
            ds18b20_init();
            s_ds18b20_last_rescan_ms = now_ms;
            s_conv_pending_read = false;
            s_last_t_in_valid = ds18b20_sensor_present(0) ? s_last_t_in_valid : false;
            s_last_t_out_valid = ds18b20_sensor_present(1) ? s_last_t_out_valid : false;
        }

        t_in = s_last_t_in_c;
        t_out = s_last_t_out_c;
        v_in = s_last_t_in_valid;
        v_out = s_last_t_out_valid;
        bool ds_ready = s_conv_pending_read &&
                        ((now_ms - s_ds18b20_request_ms) >= DS18B20_CONVERSION_MS);
        if (ds_ready) {
            if (ds18b20_sensor_present(0)) {
                v_in = ds18b20_read_temperature_c(0, &t_in);
                if (v_in) {
                    s_last_t_in_c = t_in;
                }
                s_last_t_in_valid = v_in;
            } else {
                v_in = false;
                s_last_t_in_valid = false;
            }
            if (ds18b20_sensor_present(1)) {
                v_out = ds18b20_read_temperature_c(1, &t_out);
                if (v_out) {
                    s_last_t_out_c = t_out;
                }
                s_last_t_out_valid = v_out;
            } else {
                v_out = false;
                s_last_t_out_valid = false;
            }
            s_conv_pending_read = false;
        }
        if (!s_conv_pending_read) {
            ds18b20_request_conversion();
            s_ds18b20_request_ms = now_ms;
            s_conv_pending_read = true;
        }
#elif CONFIG_IDMS_TEMP_SENSOR_MAX31865
        v_in = max31865_read_temperature_c(0, &t_in);
        v_out = max31865_read_temperature_c(1, &t_out);
#elif CONFIG_IDMS_TEMP_SENSOR_PT100_ADC
        v_in = pt100_adc_read_celsius(&s_pt100_in, &t_in);
        v_out = false;
#elif CONFIG_IDMS_TEMP_SENSOR_DS18B20_PT100
        {
            bool ds18b20_read_done = s_conv_pending_read &&
                                     ((now_ms - s_ds18b20_request_ms) >= DS18B20_CONVERSION_MS);
            float ds_t = 0.0f;
            bool ds_v = false;

            if (ds18b20_read_done && ds18b20_device_count() >= 1) {
                ds_v = ds18b20_read_temperature_c(0, &ds_t);
            }
            if (ds18b20_read_done) {
                s_conv_pending_read = false;
            }

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
            if (!s_conv_pending_read) {
                ds18b20_request_conversion();
                s_ds18b20_request_ms = now_ms;
                s_conv_pending_read = true;
            }
        }
#endif

        if (v_in) {
            t_in += (float)config_get_tin_offset_x10() / 10.0f;
        }
        if (v_out) {
            t_out += (float)config_get_tout_offset_x10() / 10.0f;
        }

        float dt = 0.0f;
        bool v_dt = v_in && v_out;
        if (v_dt) {
            dt = t_out - t_in;
        }
        float power_loss_threshold = (float)config_get_power_loss_current_ma() / 1000.0f;
        float running_current_threshold = (float)config_get_machine_running_current_ma() / 1000.0f;
        if (power_loss_threshold <= 0.0f) {
            power_loss_threshold = (float)CONFIG_IDMS_CURRENT_THRESHOLD_MA / 1000.0f;
        }
        if (running_current_threshold <= 0.0f) {
            running_current_threshold = power_loss_threshold;
        }
        int16_t dt_low = config_get_dt_alert_threshold();
        int16_t dt_high = config_get_dt_high_threshold();
        if (dt_high < dt_low) {
            dt_high = dt_low;
        }
        bool machine_running = s_adc_sensor_connected && amps >= running_current_threshold;
        bool delta_alert_active = machine_running && v_dt &&
                                  (dt < (float)dt_low || dt > (float)dt_high);

        uint32_t runtime_sensor_flags = s_sensor_init_error_flags;
        if (!s_adc) {
            runtime_sensor_flags |= IDMS_SENSOR_ERR_CURRENT_ADC;
        } else if (!s_adc_sensor_connected) {
            runtime_sensor_flags |= IDMS_SENSOR_ERR_CURRENT_RANGE;
        }
#if CONFIG_IDMS_TEMP_SENSOR_DS18B20 || CONFIG_IDMS_TEMP_SENSOR_MAX31865
        if (!v_in) {
            runtime_sensor_flags |= IDMS_SENSOR_ERR_TEMP_IN | IDMS_SENSOR_ERR_TEMP_DELTA;
        }
        if (!v_out) {
            runtime_sensor_flags |= IDMS_SENSOR_ERR_TEMP_OUT | IDMS_SENSOR_ERR_TEMP_DELTA;
        }
#elif CONFIG_IDMS_TEMP_SENSOR_PT100_ADC
        if (!v_in) {
            runtime_sensor_flags |= IDMS_SENSOR_ERR_TEMP_IN;
        }
#elif CONFIG_IDMS_TEMP_SENSOR_DS18B20_PT100
    #if CONFIG_IDMS_COMBO_T_IN_DS18B20 || CONFIG_IDMS_COMBO_T_IN_PT100
        if (!v_in) {
            runtime_sensor_flags |= IDMS_SENSOR_ERR_TEMP_IN | IDMS_SENSOR_ERR_TEMP_DELTA;
        }
    #endif
    #if CONFIG_IDMS_COMBO_T_OUT_DS18B20 || CONFIG_IDMS_COMBO_T_OUT_PT100
        if (!v_out) {
            runtime_sensor_flags |= IDMS_SENSOR_ERR_TEMP_OUT | IDMS_SENSOR_ERR_TEMP_DELTA;
        }
    #endif
#endif
        set_sensor_status_flags(runtime_sensor_flags);

#if CONFIG_IDMS_UI_ENABLE
        bool wifi_up = wifi_manager_is_connected();
#endif
#if CONFIG_LWIP_SNTP_MAX_SERVERS > 0
        bool time_synced = (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED);
        time_t timestamp_utc = time_synced ? time(NULL) : 0;
#endif

        portENTER_CRITICAL(&s_metrics_lock);
        s_metrics.current_a = amps;
        s_metrics.current_valid = s_adc_sensor_connected;
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
        if (s_adc_sensor_connected && amps < power_loss_threshold) {
            s_power_low_ms += MONITOR_PERIOD_MS;
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

        /* Cooling fault detection: only meaningful while the machine is running. */
        if (machine_running && v_dt) {
            bool bad_low = dt < (float)dt_low;
            bool bad_high = dt > (float)dt_high;
            bool bad = bad_low || bad_high;

            if (bad) {
                s_cool_bad_ms += MONITOR_PERIOD_MS;
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
                if (!machine_running) {
                    ESP_LOGI(TAG, "Machine current below %.1f A, clearing cooling fault state",
                             (double)running_current_threshold);
                } else {
                    ESP_LOGW(TAG, "Temperature sensor data invalid, resetting cooling fault state");
                }
#endif
            }
            s_cool_bad_ms = 0;
            s_cool_st = COOL_OK;
        }

        portENTER_CRITICAL(&s_metrics_lock);
        s_metrics.power_fault = (s_power_st == POWER_FAULT);
        s_metrics.cooling_fault = (s_cool_st == COOL_FAULT_LOW || s_cool_st == COOL_FAULT_HIGH);
        s_metrics.delta_alert = delta_alert_active;
        s_metrics.sensor_preflight_done = true;
        s_metrics.sensor_preflight_ok = (s_sensor_error_flags == 0);
        s_metrics.sensor_error_flags = s_sensor_error_flags;
        strncpy(s_metrics.sensor_status, s_sensor_status, sizeof(s_metrics.sensor_status) - 1);
        s_metrics.sensor_status[sizeof(s_metrics.sensor_status) - 1] = '\0';
        portEXIT_CRITICAL(&s_metrics_lock);

#if CONFIG_IDMS_UI_ENABLE
        /* Wi-Fi dependent operations: heartbeat and flush pending alerts */
        if (wifi_up) {
            if (s_sensor_preflight_alert_pending) {
                char msg[160];
                snprintf(msg, sizeof(msg), "ALERT: Sensor fault: %s (flags=0x%02lx)",
                         s_sensor_status, (unsigned long)s_sensor_error_flags);
                telegram_broadcast_alert(msg);
                s_sensor_preflight_alert_pending = false;
            }

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

esp_err_t monitor_init(void)
{
    const gpio_num_t adc_gpio = (gpio_num_t)CONFIG_IDMS_ADC_GPIO;
    adc_unit_t unit_id = ADC_UNIT_1;
    adc_channel_t ch = ADC_CHANNEL_0;
    uint32_t sensor_flags = 0;
    esp_err_t init_result = ESP_OK;

    memset(&s_metrics, 0, sizeof(s_metrics));
    s_sensor_error_flags = 0;
    s_sensor_init_error_flags = 0;
    s_sensor_last_logged_flags = UINT32_MAX;
    s_last_t_in_valid = false;
    s_last_t_out_valid = false;
    s_adc_sensor_connected = false;
    s_current_zero_valid = false;
    s_current_recovery_good_count = 0;
    s_current_ema_initialized = false;
    mark_sensor_preflight_pending();
    esp_err_t err = adc_oneshot_io_to_channel(adc_gpio, &unit_id, &ch);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADC GPIO %d is not valid for this chip (%s). Fix IDMS_ADC_GPIO in menuconfig.",
                 (int)adc_gpio, esp_err_to_name(err));
        sensor_flags |= IDMS_SENSOR_ERR_CURRENT_ADC;
        s_sensor_init_error_flags |= IDMS_SENSOR_ERR_CURRENT_ADC;
        init_result = err;
    } else {
        s_adc_channel = ch;
    }

    if ((sensor_flags & IDMS_SENSOR_ERR_CURRENT_ADC) == 0) {
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << adc_gpio,
        .mode = GPIO_MODE_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    adc_oneshot_unit_init_cfg_t ucfg = {
        .unit_id = unit_id,
    };
    err = adc_oneshot_new_unit(&ucfg, &s_adc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADC unit init failed: %s", esp_err_to_name(err));
        sensor_flags |= IDMS_SENSOR_ERR_CURRENT_ADC;
        s_sensor_init_error_flags |= IDMS_SENSOR_ERR_CURRENT_ADC;
        init_result = err;
    }

    adc_oneshot_chan_cfg_t chcfg = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = ADC_ATTEN_DB_12,
    };
    if ((sensor_flags & IDMS_SENSOR_ERR_CURRENT_ADC) == 0) {
        err = adc_oneshot_config_channel(s_adc, s_adc_channel, &chcfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ADC channel config failed: %s", esp_err_to_name(err));
            sensor_flags |= IDMS_SENSOR_ERR_CURRENT_ADC;
            s_sensor_init_error_flags |= IDMS_SENSOR_ERR_CURRENT_ADC;
            init_result = err;
        } else {
            init_current_adc_calibration(unit_id, s_adc_channel, ADC_ATTEN_DB_12,
                                         ADC_BITWIDTH_DEFAULT);
        }
    }

    if ((sensor_flags & IDMS_SENSOR_ERR_CURRENT_ADC) == 0) {
        current_sample_stats_t warmup;
        err = sample_current_stats(CURRENT_ADC_WARMUP_SAMPLES, pdMS_TO_TICKS(1), &warmup);
        if (err != ESP_OK || warmup.read_errors > 0) {
            ESP_LOGE(TAG, "ADC warm-up failed: err=%s read_errors=%d/%d",
                     esp_err_to_name(err), warmup.read_errors, CURRENT_ADC_WARMUP_SAMPLES);
            sensor_flags |= IDMS_SENSOR_ERR_CURRENT_ADC;
            init_result = err;
        } else {
            ESP_LOGI(TAG, "ADC warm-up: GPIO%d, unit=%d, ch=%d, mean=%.1f, rms=%.1f",
                     (int)adc_gpio, (int)unit_id, (int)ch,
                     (double)warmup.mean_raw, (double)warmup.rms_raw);
            if (!current_adc_bias_ok(warmup.mean_raw)) {
                ESP_LOGW(TAG, "ADC bias outside expected range %.0f-%.0f counts",
                         (double)CURRENT_ADC_BIAS_MIN_RAW,
                         (double)CURRENT_ADC_BIAS_MAX_RAW);
                ESP_LOGW(TAG, "  Check SCT-013 bias: 2x10k divider, 10uF cap, GPIO%d TP_ADC about 1.65V",
                         (int)adc_gpio);
            }
        }
    }
    }

#if CONFIG_IDMS_SCT_AUTOZERO_ENABLE
    if ((sensor_flags & IDMS_SENSOR_ERR_CURRENT_ADC) == 0) {
        err = monitor_calibrate_zero();
        if (err != ESP_OK) {
            if (err == ESP_ERR_INVALID_STATE) {
                sensor_flags |= IDMS_SENSOR_ERR_CURRENT_RANGE;
            } else {
                sensor_flags |= IDMS_SENSOR_ERR_CURRENT_CAL;
            }
            init_result = err;
        }
    }
#endif

#if CONFIG_IDMS_TEMP_SENSOR_DS18B20
    ds18b20_init();
    ESP_LOGI(TAG, "Temperature sensors: DS18B20 1-Wire (T_in=GPIO%d, T_out=GPIO%d)",
             CONFIG_IDMS_PIN_ONEWIRE, CONFIG_IDMS_PIN_ONEWIRE2);
#elif CONFIG_IDMS_TEMP_SENSOR_MAX31865
    err = max31865_init(IDMS_SENSOR_SPI_HOST);
    if (err != ESP_OK) {
        sensor_flags |= IDMS_SENSOR_ERR_TEMP_INIT;
        s_sensor_init_error_flags |= IDMS_SENSOR_ERR_TEMP_INIT;
        init_result = err;
    }
    ESP_LOGI(TAG, "Temperature sensors: PT100 via MAX31865 (CS0=GPIO%d, CS1=GPIO%d)",
             CONFIG_IDMS_MAX31865_CS0, CONFIG_IDMS_MAX31865_CS1);
#elif CONFIG_IDMS_TEMP_SENSOR_PT100_ADC
    err = pt100_adc_init(&s_pt100_in, CONFIG_IDMS_PIN_PT100_ADC,
                         (float)CONFIG_IDMS_PT100_RREF_X10 / 10.0f,
                         (float)CONFIG_IDMS_PT100_R0_X10 / 10.0f);
    if (err != ESP_OK) {
        sensor_flags |= IDMS_SENSOR_ERR_TEMP_INIT | IDMS_SENSOR_ERR_TEMP_IN;
        s_sensor_init_error_flags |= IDMS_SENSOR_ERR_TEMP_INIT;
        init_result = err;
    }
    ESP_LOGI(TAG, "Temperature sensors: PT100 via ADC (GPIO%d, R_ref=%.1fΩ)",
             CONFIG_IDMS_PIN_PT100_ADC, (float)CONFIG_IDMS_PT100_RREF_X10 / 10.0f);
#elif CONFIG_IDMS_TEMP_SENSOR_DS18B20_PT100
    ds18b20_init();
    err = pt100_adc_init(&s_pt100_in, CONFIG_IDMS_PIN_PT100_ADC,
                         (float)CONFIG_IDMS_PT100_RREF_X10 / 10.0f,
                         (float)CONFIG_IDMS_PT100_R0_X10 / 10.0f);
    if (err != ESP_OK) {
        sensor_flags |= IDMS_SENSOR_ERR_TEMP_INIT;
        s_sensor_init_error_flags |= IDMS_SENSOR_ERR_TEMP_INIT;
        init_result = err;
    }
#if CONFIG_IDMS_PIN_PT100_ADC2 >= 0
    err = pt100_adc_init(&s_pt100_out, CONFIG_IDMS_PIN_PT100_ADC2,
                         (float)CONFIG_IDMS_PT100_RREF_X10 / 10.0f,
                         (float)CONFIG_IDMS_PT100_R0_X10 / 10.0f);
    if (err != ESP_OK) {
        sensor_flags |= IDMS_SENSOR_ERR_TEMP_INIT;
        s_sensor_init_error_flags |= IDMS_SENSOR_ERR_TEMP_INIT;
        init_result = err;
    }
#endif
    ESP_LOGI(TAG, "Temperature sensors: DS18B20+PT100 combo (1-Wire=GPIO%d/%d, PT100 ADC=GPIO%d)",
             CONFIG_IDMS_PIN_ONEWIRE, CONFIG_IDMS_PIN_ONEWIRE2, CONFIG_IDMS_PIN_PT100_ADC);
#endif

    if ((sensor_flags & IDMS_SENSOR_ERR_CURRENT_ADC) == 0) {
        sensor_flags |= preflight_current_sensor();
    }
    sensor_flags |= preflight_temperature_sensors();
    set_sensor_status_flags(sensor_flags);

#if CONFIG_IDMS_UI_ENABLE
    s_sensor_preflight_alert_pending = (sensor_flags != 0);
#endif

    BaseType_t task_ok = xTaskCreatePinnedToCore(monitor_task, "monitor", 8192, NULL, 5, NULL,
                                                 tskNO_AFFINITY);
    if (task_ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to start monitor task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Monitor task started (SCT-013 ADC on GPIO%d, adc unit %d channel %d, sensor_status=%s)",
             (int)adc_gpio, (int)unit_id, (int)s_adc_channel,
             sensor_flags == 0 ? "OK" : s_sensor_status);

    if (sensor_flags != 0 && init_result == ESP_OK) {
        init_result = ESP_ERR_INVALID_STATE;
    }
    return init_result;
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

void monitor_reset_current_filter(void)
{
    s_current_ema = 0.0f;
    s_current_ema_initialized = false;
}

void monitor_adc_debug(int *out_mean, int *out_rms, int *out_errors)
{
    const int n = 64;
    int32_t sum = 0;
    int64_t sum_sq = 0;
    int err = 0;

    if (!s_adc) {
        if (out_mean) *out_mean = 0;
        if (out_rms) *out_rms = 0;
        if (out_errors) *out_errors = n;
        return;
    }

    for (int i = 0; i < n; i++) {
        int v = 0;
        esp_err_t ret = adc_oneshot_read(s_adc, s_adc_channel, &v);
        if (ret != ESP_OK) {
            v = 0;
            err++;
        }
        sum += v;
        sum_sq += (int64_t)v * (int64_t)v;
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    float mean = (float)sum / (float)n;
    float mean_sq = (float)sum_sq / (float)n;
    float variance = mean_sq - (mean * mean);
    if (variance < 0.0f) variance = 0.0f;

    if (out_mean) *out_mean = (int)mean;
    if (out_rms) *out_rms = (int)sqrtf(variance);
    if (out_errors) *out_errors = err;
}
