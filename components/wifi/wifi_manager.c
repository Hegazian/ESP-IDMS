#include "wifi_manager.h"
#include "config_store.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "wifi";

static esp_netif_t *s_netif;
static bool s_connected;
static char s_ip[16];

static void set_ip(void)
{
    esp_netif_ip_info_t ip;
    if (s_netif && esp_netif_get_ip_info(s_netif, &ip) == ESP_OK) {
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ip.ip));
    } else {
        s_ip[0] = '\0';
    }
}

static void try_connect(void)
{
    char ssid[64] = {0};
    char password[64] = {0};
    config_get_wifi_ssid(ssid, sizeof(ssid));
    config_get_wifi_password(password, sizeof(password));

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

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        try_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        wifi_event_sta_disconnected_t *ev = (wifi_event_sta_disconnected_t *)data;
        ESP_LOGW(TAG, "Disconnected (reason=%d), reconnecting…", ev->reason);
        try_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_connected = true;
        set_ip();
        ESP_LOGI(TAG, "Got IP: %s", s_ip);
    }
}

esp_err_t wifi_manager_init(void)
{
    s_connected = false;
    s_ip[0] = '\0';

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

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
