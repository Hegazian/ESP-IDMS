#include "mqtt_service.h"
#include "config_store.h"
#include "monitor.h"
#include "network_manager.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "mqtt";
static esp_mqtt_client_handle_t client = NULL;
static bool connected = false;

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED: {
            ESP_LOGI(TAG, "MQTT connected");
            connected = true;
            char client_id[64];
            if (config_get_mqtt_client_id(client_id, sizeof(client_id)) == ESP_OK) {
                char topic[128];
                snprintf(topic, sizeof(topic), "idms/%s/cmd", client_id);
                esp_mqtt_client_subscribe(client, topic, 1);
            }
            break;
        }
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "MQTT disconnected");
            connected = false;
            break;
        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "MQTT data received on topic: %.*s", event->topic_len, event->topic);
            if (event->data_len >= 6 && strncmp(event->data, "reboot", 6) == 0) {
                esp_restart();
            }
            break;
        default:
            break;
    }
}

static void mqtt_task(void *arg) {
    char topic[128];
    char client_id[64];
    if (config_get_mqtt_client_id(client_id, sizeof(client_id)) != ESP_OK) {
        strcpy(client_id, "idms_default");
    }
    snprintf(topic, sizeof(topic), "idms/%s/telemetry", client_id);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        if (connected) {
            idms_metrics_t m;
            monitor_get_metrics(&m);

            cJSON *root = cJSON_CreateObject();
            cJSON_AddNumberToObject(root, "t_in", m.t_in_c);
            cJSON_AddNumberToObject(root, "t_out", m.t_out_c);
            cJSON_AddNumberToObject(root, "current", m.current_a);
            cJSON_AddBoolToObject(root, "power_fault", m.power_fault);
            cJSON_AddBoolToObject(root, "cooling_fault", m.cooling_fault);
            cJSON_AddBoolToObject(root, "delta_alert", m.delta_alert);

            char *json_str = cJSON_PrintUnformatted(root);
            if (json_str) {
                esp_mqtt_client_publish(client, topic, json_str, 0, 1, 0);
                free(json_str);
            }
            cJSON_Delete(root);
        }
    }
}

esp_err_t mqtt_service_init(void) {
#if CONFIG_IDMS_MQTT_ENABLE
    char uri[128];
    char user[64];
    char pass[64];
    char client_id[64];

    config_get_mqtt_uri(uri, sizeof(uri));
    config_get_mqtt_user(user, sizeof(user));
    config_get_mqtt_pass(pass, sizeof(pass));
    config_get_mqtt_client_id(client_id, sizeof(client_id));

    if (uri[0] == '\0') {
        ESP_LOGI(TAG, "MQTT URI not set, disabling MQTT");
        return ESP_OK;
    }

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = uri,
        .credentials.username = user[0] ? user : NULL,
        .credentials.authentication.password = pass[0] ? pass : NULL,
        .credentials.client_id = client_id
    };

    client = esp_mqtt_client_init(&mqtt_cfg);
    if (!client) {
        return ESP_FAIL;
    }

    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);

    xTaskCreate(mqtt_task, "mqtt_pub", 4096, NULL, 4, NULL);

    ESP_LOGI(TAG, "MQTT initialized (URI: %s)", uri);
#endif
    return ESP_OK;
}
