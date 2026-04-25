#include "wifi_manager.h"
#include "config_store.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include <stdio.h>
#include <string.h>

#if CONFIG_LWIP_SNTP_MAX_SERVERS > 0
#include "esp_sntp.h"
#endif

static const char *TAG = "wifi";

static esp_netif_t *s_netif;
static bool s_connected;
static char s_ip[16];
static int s_reconnect_attempts = 0;
static TimerHandle_t s_reconnect_timer = NULL;

#define MAX_RECONNECT_DELAY_MS 60000  /* 60 seconds max */
#define INITIAL_RECONNECT_DELAY_MS 1000

static void set_ip(void)
{
    esp_netif_ip_info_t ip;
    if (s_netif && esp_netif_get_ip_info(s_netif, &ip) == ESP_OK) {
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ip.ip));
    } else {
        s_ip[0] = '\0';
    }
}

static int get_reconnect_delay(void)
{
    /* Exponential backoff: 1s, 2s, 4s, 8s, 16s, 32s, 60s (max) */
    int delay = INITIAL_RECONNECT_DELAY_MS << (s_reconnect_attempts < 6 ? s_reconnect_attempts : 6);
    return (delay > MAX_RECONNECT_DELAY_MS) ? MAX_RECONNECT_DELAY_MS : delay;
}

static void try_connect(void)
{
    char ssid[64] = {0};
    char password[64] = {0};
    config_get_wifi_ssid(ssid, sizeof(ssid));
    config_get_wifi_password(password, sizeof(password));

    /* Validate credentials */
    if (ssid[0] == '\0') {
        ESP_LOGW(TAG, "WiFi SSID not configured!");
        return;
    }

    wifi_config_t cfg = {0};
    strncpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid) - 1);
    strncpy((char *)cfg.sta.password, password, sizeof(cfg.sta.password) - 1);
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK) {
        /* WiFi may be stopping (during reboot) — ignore gracefully */
        if (err != ESP_ERR_WIFI_NOT_STARTED && err != ESP_ERR_WIFI_STOP_STATE) {
            ESP_LOGW(TAG, "esp_wifi_set_config: %s", esp_err_to_name(err));
        }
        return;
    }
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
    }
}

static void reconnect_timer_callback(TimerHandle_t timer)
{
    ESP_LOGI(TAG, "Reconnect attempt %d", s_reconnect_attempts + 1);
    try_connect();
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        s_reconnect_attempts = 0;
        try_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        s_ip[0] = '\0';
        wifi_event_sta_disconnected_t *ev = (wifi_event_sta_disconnected_t *)data;
        
        /* Calculate exponential backoff delay */
        int delay_ms = get_reconnect_delay();
        s_reconnect_attempts++;
        
        ESP_LOGW(TAG, "Disconnected (reason=%d), reconnecting in %d ms (attempt %d)", 
                 ev->reason, delay_ms, s_reconnect_attempts);
        
        /* Start/restart reconnect timer */
        if (s_reconnect_timer) {
            xTimerChangePeriod(s_reconnect_timer, pdMS_TO_TICKS(delay_ms), 0);
            xTimerReset(s_reconnect_timer, 0);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_connected = true;
        s_reconnect_attempts = 0;  /* Reset backoff on successful connection */
        if (s_reconnect_timer) {
            xTimerStop(s_reconnect_timer, 0);
        }
        set_ip();
        ESP_LOGI(TAG, "Got IP: %s", s_ip);

#if CONFIG_LWIP_SNTP_MAX_SERVERS > 0
        if (esp_sntp_enabled() == 0) {
            esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
            esp_sntp_setservername(0, "pool.ntp.org");
            esp_sntp_setservername(1, "time.google.com");
            esp_sntp_setservername(2, "time.windows.com");
            esp_sntp_init();
            ESP_LOGI(TAG, "SNTP initialized");
        }
#endif
    }
}

esp_err_t wifi_manager_init(void)
{
    s_connected = false;
    s_ip[0] = '\0';
    s_reconnect_attempts = 0;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    /* Create reconnect timer for exponential backoff */
    s_reconnect_timer = xTimerCreate(
        "wifi_reconnect",
        pdMS_TO_TICKS(INITIAL_RECONNECT_DELAY_MS),
        pdFALSE,  /* One-shot timer */
        NULL,
        reconnect_timer_callback
    );
    if (!s_reconnect_timer) {
        ESP_LOGE(TAG, "Failed to create reconnect timer");
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    return ESP_OK;
}

bool wifi_manager_is_connected(void)
{
    return s_connected;
}

void wifi_manager_get_ip(char *out, size_t len)
{
    if (!out || len == 0) {
        return;
    }
    if (s_connected) {
        set_ip();
    }
    strncpy(out, s_ip, len - 1);
    out[len - 1] = '\0';
}
