#include "telemetry.h"
#include "monitor.h"
#include "esp_spiffs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include <float.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "telemetry";

#define TELEMETRY_SAMPLE_PERIOD_MS 60000
#define TELEMETRY_WEEK_SECONDS     (7 * 24 * 60 * 60)
#define TELEMETRY_REPORT_HOUR_UTC  9
#define TELEMETRY_REPORT_WDAY      1  /* Monday, struct tm: Sunday=0 */
#define TELEMETRY_NVS_NS           "telemetry"
#define TELEMETRY_NVS_SPIFFS_READY "spiffs_ready"
#define TELEMETRY_BASE_PATH        "/spiffs"
#define TELEMETRY_CSV_PATH         TELEMETRY_BASE_PATH "/telemetry.csv"
#define TELEMETRY_CSV_MAX_BYTES    (512 * 1024)

typedef struct {
    uint32_t samples;
    uint32_t current_valid_samples;
    uint32_t t_in_valid_samples;
    uint32_t t_out_valid_samples;
    uint32_t delta_valid_samples;
    double current_sum;
    double t_in_sum;
    double t_out_sum;
    double delta_sum;
    float current_min;
    float current_max;
    float t_in_min;
    float t_in_max;
    float t_out_min;
    float t_out_max;
    float delta_min;
    float delta_max;
    uint32_t power_fault_events;
    uint32_t cooling_fault_events;
    uint32_t sensor_fault_events;
    time_t since_utc;
    time_t last_sample_utc;
    bool time_synced;
    bool prev_state_valid;
    bool prev_power_fault;
    bool prev_cooling_fault;
    bool prev_sensor_fault;
} telemetry_state_t;

static telemetry_state_t s_week;
static SemaphoreHandle_t s_mux;
static SemaphoreHandle_t s_file_mux;
static TaskHandle_t s_task;
static bool s_last_report_loaded;
static int32_t s_last_report_week_id = -1;
static bool s_storage_ready;

static void telemetry_task(void *arg);
static esp_err_t init_storage(void);
static void append_csv_sample(const idms_metrics_t *m, time_t now);

static bool storage_marked_ready(void)
{
    nvs_handle_t h;
    uint8_t ready = 0;
    if (nvs_open(TELEMETRY_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_get_u8(h, TELEMETRY_NVS_SPIFFS_READY, &ready);
    nvs_close(h);
    return err == ESP_OK && ready == 1;
}

static void mark_storage_ready(void)
{
    nvs_handle_t h;
    if (nvs_open(TELEMETRY_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, TELEMETRY_NVS_SPIFFS_READY, 1);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void reset_week_locked(time_t now)
{
    memset(&s_week, 0, sizeof(s_week));
    s_week.current_min = FLT_MAX;
    s_week.t_in_min = FLT_MAX;
    s_week.t_out_min = FLT_MAX;
    s_week.delta_min = FLT_MAX;
    s_week.current_max = -FLT_MAX;
    s_week.t_in_max = -FLT_MAX;
    s_week.t_out_max = -FLT_MAX;
    s_week.delta_max = -FLT_MAX;
    s_week.since_utc = now;
}

static void add_metric(float value, double *sum, float *min_v, float *max_v, uint32_t *count)
{
    *sum += value;
    if (value < *min_v) *min_v = value;
    if (value > *max_v) *max_v = value;
    (*count)++;
}

static float avg_or_zero(double sum, uint32_t count)
{
    return count > 0 ? (float)(sum / (double)count) : 0.0f;
}

static float min_or_zero(float value, uint32_t count)
{
    return count > 0 ? value : 0.0f;
}

static float max_or_zero(float value, uint32_t count)
{
    return count > 0 ? value : 0.0f;
}

static void sample_once(void)
{
    idms_metrics_t m;
    monitor_get_metrics(&m);

    time_t now = m.time_synced ? m.timestamp_utc : time(NULL);
    if (now <= 0) {
        now = (time_t)(xTaskGetTickCount() * portTICK_PERIOD_MS / 1000);
    }

    if (!s_mux || xSemaphoreTake(s_mux, pdMS_TO_TICKS(500)) != pdTRUE) {
        return;
    }

    if (s_week.since_utc == 0 || (m.time_synced && s_week.time_synced && now - s_week.since_utc >= TELEMETRY_WEEK_SECONDS)) {
        reset_week_locked(now);
    }

    s_week.samples++;
    s_week.last_sample_utc = now;
    if (m.time_synced) {
        s_week.time_synced = true;
    }

    if (m.current_valid) {
        add_metric(m.current_a, &s_week.current_sum, &s_week.current_min,
                   &s_week.current_max, &s_week.current_valid_samples);
    }
    if (m.t_in_valid) {
        add_metric(m.t_in_c, &s_week.t_in_sum, &s_week.t_in_min,
                   &s_week.t_in_max, &s_week.t_in_valid_samples);
    }
    if (m.t_out_valid) {
        add_metric(m.t_out_c, &s_week.t_out_sum, &s_week.t_out_min,
                   &s_week.t_out_max, &s_week.t_out_valid_samples);
    }
    if (m.delta_valid) {
        add_metric(m.delta_t_c, &s_week.delta_sum, &s_week.delta_min,
                   &s_week.delta_max, &s_week.delta_valid_samples);
    }

    bool sensor_fault = !m.sensor_preflight_ok || m.sensor_error_flags != 0;
    if (s_week.prev_state_valid) {
        if (!s_week.prev_power_fault && m.power_fault) s_week.power_fault_events++;
        if (!s_week.prev_cooling_fault && m.cooling_fault) s_week.cooling_fault_events++;
        if (!s_week.prev_sensor_fault && sensor_fault) s_week.sensor_fault_events++;
    }
    s_week.prev_state_valid = true;
    s_week.prev_power_fault = m.power_fault;
    s_week.prev_cooling_fault = m.cooling_fault;
    s_week.prev_sensor_fault = sensor_fault;

    xSemaphoreGive(s_mux);

    append_csv_sample(&m, now);
}

esp_err_t telemetry_init(void)
{
    if (!s_mux) {
        s_mux = xSemaphoreCreateMutex();
        if (!s_mux) return ESP_ERR_NO_MEM;
        xSemaphoreTake(s_mux, portMAX_DELAY);
        reset_week_locked(time(NULL));
        xSemaphoreGive(s_mux);
    }
    if (!s_file_mux) {
        s_file_mux = xSemaphoreCreateMutex();
        if (!s_file_mux) return ESP_ERR_NO_MEM;
    }

    esp_err_t storage_err = init_storage();
    s_storage_ready = (storage_err == ESP_OK);
    if (storage_err != ESP_OK) {
        ESP_LOGW(TAG, "Telemetry CSV storage unavailable: %s", esp_err_to_name(storage_err));
    }

    if (!s_task) {
        BaseType_t ok = xTaskCreatePinnedToCore(telemetry_task, "telemetry",
                                                4096, NULL, 3, &s_task, tskNO_AFFINITY);
        if (ok != pdPASS) {
            s_task = NULL;
            return ESP_ERR_NO_MEM;
        }
    }
    ESP_LOGI(TAG, "Telemetry aggregation started (sample period %u ms)",
             (unsigned)TELEMETRY_SAMPLE_PERIOD_MS);
    return s_storage_ready ? ESP_OK : storage_err;
}

const char *telemetry_csv_path(void)
{
    return TELEMETRY_CSV_PATH;
}

esp_err_t telemetry_csv_lock(uint32_t timeout_ms)
{
    if (!s_file_mux) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_file_mux, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

void telemetry_csv_unlock(void)
{
    if (s_file_mux) {
        xSemaphoreGive(s_file_mux);
    }
}

void telemetry_get_weekly_stats(telemetry_weekly_stats_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!s_mux || xSemaphoreTake(s_mux, pdMS_TO_TICKS(500)) != pdTRUE) {
        return;
    }

    out->samples = s_week.samples;
    out->current_valid_samples = s_week.current_valid_samples;
    out->t_in_valid_samples = s_week.t_in_valid_samples;
    out->t_out_valid_samples = s_week.t_out_valid_samples;
    out->delta_valid_samples = s_week.delta_valid_samples;
    out->current_avg = avg_or_zero(s_week.current_sum, s_week.current_valid_samples);
    out->current_min = min_or_zero(s_week.current_min, s_week.current_valid_samples);
    out->current_max = max_or_zero(s_week.current_max, s_week.current_valid_samples);
    out->t_in_avg = avg_or_zero(s_week.t_in_sum, s_week.t_in_valid_samples);
    out->t_in_min = min_or_zero(s_week.t_in_min, s_week.t_in_valid_samples);
    out->t_in_max = max_or_zero(s_week.t_in_max, s_week.t_in_valid_samples);
    out->t_out_avg = avg_or_zero(s_week.t_out_sum, s_week.t_out_valid_samples);
    out->t_out_min = min_or_zero(s_week.t_out_min, s_week.t_out_valid_samples);
    out->t_out_max = max_or_zero(s_week.t_out_max, s_week.t_out_valid_samples);
    out->delta_avg = avg_or_zero(s_week.delta_sum, s_week.delta_valid_samples);
    out->delta_min = min_or_zero(s_week.delta_min, s_week.delta_valid_samples);
    out->delta_max = max_or_zero(s_week.delta_max, s_week.delta_valid_samples);
    out->power_fault_events = s_week.power_fault_events;
    out->cooling_fault_events = s_week.cooling_fault_events;
    out->sensor_fault_events = s_week.sensor_fault_events;
    out->since_utc = s_week.since_utc;
    out->last_sample_utc = s_week.last_sample_utc;
    out->time_synced = s_week.time_synced;

    xSemaphoreGive(s_mux);
}

static void fmt_time(time_t t, char *out, size_t out_sz)
{
    if (!out || out_sz == 0) return;
    if (t <= 0) {
        snprintf(out, out_sz, "unknown");
        return;
    }
    struct tm tm_utc;
    gmtime_r(&t, &tm_utc);
    strftime(out, out_sz, "%Y-%m-%d %H:%M UTC", &tm_utc);
}

static esp_err_t init_storage(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = TELEMETRY_BASE_PATH,
        .partition_label = "storage",
        .max_files = 4,
        .format_if_mount_failed = false,
    };

    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err == ESP_ERR_INVALID_STATE) {
        err = ESP_OK;
    }
    if (err != ESP_OK) {
        bool already_ready = storage_marked_ready();
        ESP_LOGE(TAG, "Telemetry SPIFFS mount failed without formatting: %s", esp_err_to_name(err));
        if (!already_ready) {
            ESP_LOGW(TAG, "Telemetry storage has no ready marker; formatting blank first-boot SPIFFS partition");
            esp_err_t fmt_err = esp_spiffs_format("storage");
            if (fmt_err != ESP_OK) {
                ESP_LOGE(TAG, "Telemetry SPIFFS first-boot format failed: %s", esp_err_to_name(fmt_err));
                return err;
            }
            err = esp_vfs_spiffs_register(&conf);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Telemetry SPIFFS mount after format failed: %s", esp_err_to_name(err));
                return err;
            }
        } else {
            return err;
        }
    }

    size_t total = 0;
    size_t used = 0;
    if (esp_spiffs_info("storage", &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "Telemetry SPIFFS mounted: %lu/%lu bytes used",
                 (unsigned long)used, (unsigned long)total);
    }

    mark_storage_ready();
    return ESP_OK;
}

static void ensure_csv_header(FILE *f)
{
    if (!f) return;
    long pos = ftell(f);
    if (pos == 0) {
        fprintf(f,
                "timestamp_utc,current_a,current_valid,t_in_c,t_in_valid,"
                "t_out_c,t_out_valid,delta_t_c,delta_valid,power_fault,"
                "cooling_fault,sensor_error_flags,wifi_connected\n");
    }
}

static void rotate_csv_if_needed(void)
{
    struct stat st;
    if (stat(TELEMETRY_CSV_PATH, &st) == 0 && st.st_size > TELEMETRY_CSV_MAX_BYTES) {
        remove(TELEMETRY_BASE_PATH "/telemetry.old.csv");
        rename(TELEMETRY_CSV_PATH, TELEMETRY_BASE_PATH "/telemetry.old.csv");
    }
}

static void append_csv_sample(const idms_metrics_t *m, time_t now)
{
    if (!m || !s_file_mux || !s_storage_ready) return;
    if (telemetry_csv_lock(1000) != ESP_OK) {
        return;
    }

    rotate_csv_if_needed();

    FILE *f = fopen(TELEMETRY_CSV_PATH, "a");
    if (!f) {
        telemetry_csv_unlock();
        return;
    }

    ensure_csv_header(f);
    fprintf(f,
            "%lld,%.3f,%u,%.3f,%u,%.3f,%u,%.3f,%u,%u,%u,0x%08lx,%u\n",
            (long long)now,
            (double)m->current_a, m->current_valid ? 1u : 0u,
            (double)m->t_in_c, m->t_in_valid ? 1u : 0u,
            (double)m->t_out_c, m->t_out_valid ? 1u : 0u,
            (double)m->delta_t_c, m->delta_valid ? 1u : 0u,
            m->power_fault ? 1u : 0u,
            m->cooling_fault ? 1u : 0u,
            (unsigned long)m->sensor_error_flags,
            m->wifi_connected ? 1u : 0u);
    fclose(f);
    telemetry_csv_unlock();
}

void telemetry_build_weekly_report(char *buf, size_t buf_sz)
{
    if (!buf || buf_sz == 0) return;

    telemetry_weekly_stats_t s;
    telemetry_get_weekly_stats(&s);

    char since[32];
    char last[32];
    fmt_time(s.since_utc, since, sizeof(since));
    fmt_time(s.last_sample_utc, last, sizeof(last));

    if (s.samples == 0) {
        snprintf(buf, buf_sz,
                 "<b>ESP-IDMS Weekly Report</b>\n\n"
                 "No telemetry samples collected yet.\n"
                 "The first report will populate after the telemetry sampler runs.");
        return;
    }

    snprintf(buf, buf_sz,
             "<b>ESP-IDMS Weekly Report</b>\n\n"
             "<b>Window:</b> %s\n"
             "<b>Last sample:</b> %s\n"
             "<b>Samples:</b> %lu\n"
             "<b>Time sync:</b> %s\n\n"
             "<b>Current:</b> avg %.2f A, min %.2f, max %.2f (%lu valid)\n"
             "<b>T_in:</b> avg %.1f C, min %.1f, max %.1f (%lu valid)\n"
             "<b>T_out:</b> avg %.1f C, min %.1f, max %.1f (%lu valid)\n"
             "<b>Delta T:</b> avg %.1f C, min %.1f, max %.1f (%lu valid)\n\n"
             "<b>Fault events:</b>\n"
             "Power: %lu\n"
             "Cooling: %lu\n"
             "Sensors: %lu",
             since, last, (unsigned long)s.samples,
             s.time_synced ? "OK" : "not synced",
             (double)s.current_avg, (double)s.current_min, (double)s.current_max,
             (unsigned long)s.current_valid_samples,
             (double)s.t_in_avg, (double)s.t_in_min, (double)s.t_in_max,
             (unsigned long)s.t_in_valid_samples,
             (double)s.t_out_avg, (double)s.t_out_min, (double)s.t_out_max,
             (unsigned long)s.t_out_valid_samples,
             (double)s.delta_avg, (double)s.delta_min, (double)s.delta_max,
             (unsigned long)s.delta_valid_samples,
             (unsigned long)s.power_fault_events,
             (unsigned long)s.cooling_fault_events,
             (unsigned long)s.sensor_fault_events);
}

static int32_t week_id_from_time(time_t now)
{
    struct tm tm_utc;
    gmtime_r(&now, &tm_utc);
    return (int32_t)((tm_utc.tm_year + 1900) * 100 + (tm_utc.tm_yday / 7));
}

static void load_last_report_week(void)
{
    if (s_last_report_loaded) return;
    s_last_report_loaded = true;

    nvs_handle_t h;
    if (nvs_open(TELEMETRY_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_i32(h, "weekly_id", &s_last_report_week_id);
        nvs_close(h);
    }
}

bool telemetry_weekly_report_due(void)
{
    load_last_report_week();

    bool synced = false;
    if (s_mux && xSemaphoreTake(s_mux, pdMS_TO_TICKS(200)) == pdTRUE) {
        synced = s_week.time_synced;
        xSemaphoreGive(s_mux);
    }
    if (!synced) return false;

    time_t now = time(NULL);
    if (now <= 0) return false;

    struct tm tm_utc;
    gmtime_r(&now, &tm_utc);
    if (tm_utc.tm_wday != TELEMETRY_REPORT_WDAY ||
        tm_utc.tm_hour != TELEMETRY_REPORT_HOUR_UTC) {
        return false;
    }

    int32_t week_id = week_id_from_time(now);
    return week_id != s_last_report_week_id;
}

void telemetry_mark_weekly_report_sent(void)
{
    time_t now = time(NULL);
    if (now <= 0) return;

    s_last_report_week_id = week_id_from_time(now);
    s_last_report_loaded = true;

    nvs_handle_t h;
    if (nvs_open(TELEMETRY_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, "weekly_id", s_last_report_week_id);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void telemetry_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(5000));
    for (;;) {
        sample_once();
        vTaskDelay(pdMS_TO_TICKS(TELEMETRY_SAMPLE_PERIOD_MS));
    }
}
