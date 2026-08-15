#include "network_manager.h"
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

#if CONFIG_IDMS_ENABLE_ETHERNET
#include "esp_eth.h"
#include "esp_mac.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_eth_enc28j60.h"
#endif

static const char *TAG = "network";

static esp_netif_t *s_wifi_netif = NULL;
static esp_netif_t *s_eth_netif = NULL;

static bool s_wifi_connected = false;
static bool s_eth_connected = false;
static char s_ip[16];
static int s_reconnect_attempts = 0;
static TimerHandle_t s_reconnect_timer = NULL;

#define MAX_RECONNECT_DELAY_MS 60000
#define INITIAL_RECONNECT_DELAY_MS 1000

static void set_ip(esp_netif_t *netif)
{
    esp_netif_ip_info_t ip;
    if (netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ip.ip));
    } else {
        s_ip[0] = '\0';
    }
}

static int get_reconnect_delay(void)
{
    int delay = INITIAL_RECONNECT_DELAY_MS << (s_reconnect_attempts < 6 ? s_reconnect_attempts : 6);
    return (delay > MAX_RECONNECT_DELAY_MS) ? MAX_RECONNECT_DELAY_MS : delay;
}

static void try_connect_wifi(void)
{
    if (s_eth_connected) {
        ESP_LOGI(TAG, "Ethernet connected, skipping Wi-Fi connect");
        return;
    }

    char ssid[64] = {0};
    char password[64] = {0};
    config_get_wifi_ssid(ssid, sizeof(ssid));
    config_get_wifi_password(password, sizeof(password));

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
    if (!s_eth_connected) {
        ESP_LOGI(TAG, "Wi-Fi reconnect attempt %d", s_reconnect_attempts + 1);
        try_connect_wifi();
    }
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        s_reconnect_attempts = 0;
        try_connect_wifi();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_connected = false;
        if (!s_eth_connected) s_ip[0] = '\0';
        wifi_event_sta_disconnected_t *ev = (wifi_event_sta_disconnected_t *)data;
        
        if (!s_eth_connected) {
            int delay_ms = get_reconnect_delay();
            s_reconnect_attempts++;
            ESP_LOGW(TAG, "Wi-Fi Disconnected (reason=%d), reconnecting in %d ms", ev->reason, delay_ms);
            if (s_reconnect_timer) {
                xTimerChangePeriod(s_reconnect_timer, pdMS_TO_TICKS(delay_ms), 0);
                xTimerReset(s_reconnect_timer, 0);
            }
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_wifi_connected = true;
        s_reconnect_attempts = 0;
        if (s_reconnect_timer) xTimerStop(s_reconnect_timer, 0);
        if (!s_eth_connected) {
            set_ip(s_wifi_netif);
            ESP_LOGI(TAG, "Wi-Fi Got IP: %s", s_ip);
        }
    }
#if CONFIG_IDMS_ENABLE_ETHERNET
    else if (base == ETH_EVENT && id == ETHERNET_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "Ethernet Link Up");
    } else if (base == ETH_EVENT && id == ETHERNET_EVENT_DISCONNECTED) {
        ESP_LOGI(TAG, "Ethernet Link Down");
        s_eth_connected = false;
        s_ip[0] = '\0';
        /* Trigger Wi-Fi fallback */
        s_reconnect_attempts = 0;
        try_connect_wifi();
    } else if (base == IP_EVENT && id == IP_EVENT_ETH_GOT_IP) {
        s_eth_connected = true;
        set_ip(s_eth_netif);
        ESP_LOGI(TAG, "Ethernet Got IP: %s", s_ip);
        /* Ethernet takes precedence; disconnect Wi-Fi to save power */
        esp_wifi_disconnect();
    }
#endif
}

esp_err_t network_manager_init(void)
{
    s_wifi_connected = false;
    s_eth_connected = false;
    s_ip[0] = '\0';
    s_reconnect_attempts = 0;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Initialize Wi-Fi */
    s_wifi_netif = esp_netif_create_default_wifi_sta();
    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

#if CONFIG_IDMS_ENABLE_ETHERNET
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &event_handler, NULL));

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    s_eth_netif = esp_netif_new(&netif_cfg);

    spi_bus_config_t buscfg = {
        .miso_io_num = CONFIG_IDMS_ETH_SPI_MISO,
        .mosi_io_num = CONFIG_IDMS_ETH_SPI_MOSI,
        .sclk_io_num = CONFIG_IDMS_ETH_SPI_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t devcfg = {
        .mode = 0,
        .clock_speed_hz = 12 * 1000 * 1000,
        .spics_io_num = CONFIG_IDMS_ETH_SPI_CS,
        .queue_size = 20,
        .cs_ena_posttrans = enc28j60_cal_spi_cs_hold_time(12),
    };

    eth_enc28j60_config_t enc28j60_config = ETH_ENC28J60_DEFAULT_CONFIG(SPI3_HOST, &devcfg);
    enc28j60_config.int_gpio_num = CONFIG_IDMS_ETH_INT;
    
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    esp_eth_mac_t *mac = esp_eth_mac_new_enc28j60(&enc28j60_config, &mac_config);

    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = -1;
    phy_config.reset_gpio_num = -1; // ENC28J60 doesn't have a separate PHY reset
    esp_eth_phy_t *phy = esp_eth_phy_new_enc28j60(&phy_config);

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth_handle = NULL;
    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_config, &eth_handle));

    uint8_t eth_mac[6];
    esp_read_mac(eth_mac, ESP_MAC_ETH);
    esp_eth_ioctl(eth_handle, ETH_CMD_S_MAC_ADDR, eth_mac);

    ESP_ERROR_CHECK(esp_netif_attach(s_eth_netif, esp_eth_new_netif_glue(eth_handle)));
    ESP_ERROR_CHECK(esp_eth_start(eth_handle));
#endif

    s_reconnect_timer = xTimerCreate("wifi_reconnect", pdMS_TO_TICKS(INITIAL_RECONNECT_DELAY_MS), pdFALSE, NULL, reconnect_timer_callback);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    return ESP_OK;
}

bool network_manager_is_connected(void)
{
    return s_wifi_connected || s_eth_connected;
}

void network_manager_get_ip(char *out, size_t len)
{
    if (!out || len == 0) return;
    strncpy(out, s_ip, len - 1);
    out[len - 1] = '\0';
}

void network_manager_reconnect(void)
{
    ESP_LOGI(TAG, "Network reconnect triggered");
    if (s_reconnect_timer) xTimerStop(s_reconnect_timer, 0);
    s_reconnect_attempts = 0;
    
    if (!s_eth_connected) {
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(100));
        try_connect_wifi();
    }
}
