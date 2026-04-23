#include "telegram.h"
#include "tg_http.h"
#include "tg_send.h"
#include "tg_bot.h"
#include "tg_parse.h"
#include "cJSON.h"
#include "esp_log.h"

static const char *TAG = "telegram";

esp_err_t telegram_send_text(const char *chat_id, const char *text)
{
    return tg_send_text(chat_id, text);
}

esp_err_t telegram_broadcast_text(const char *text)
{
    return tg_broadcast_text(text);
}

esp_err_t telegram_broadcast_alert(const char *text)
{
    return tg_broadcast_alert(text);
}

esp_err_t telegram_send_ringing_alert(const char *chat_id, const char *alert_text)
{
    return tg_send_ring_alert(chat_id, alert_text);
}

esp_err_t telegram_heartbeat(void)
{
    int st = 0;
    char resp[512] = {0};
    esp_err_t err = tg_http_get("/getMe", resp, sizeof(resp), &st);
    if (err != ESP_OK || st < 200 || st >= 300) {
        ESP_LOGW(TAG, "Heartbeat HTTP error: err=%s st=%d", esp_err_to_name(err), st);
        return err;
    }
    cJSON *root = cJSON_Parse(resp);
    if (!root) {
        ESP_LOGW(TAG, "Heartbeat: invalid JSON response");
        return ESP_FAIL;
    }
    cJSON *ok = cJSON_GetObjectItem(root, "ok");
    bool is_ok = cJSON_IsBool(ok) && cJSON_IsTrue(ok);
    cJSON_Delete(root);
    if (is_ok) {
        ESP_LOGD(TAG, "Heartbeat OK (token valid)");
    } else {
        ESP_LOGW(TAG, "Heartbeat: API returned ok=false (token invalid?)");
        err = ESP_FAIL;
    }
    return err;
}

void telegram_command_poll_start(void)
{
    tg_send_init();
    tg_bot_start();
}

void telegram_flush_offline_queue(void)
{
    tg_flush_offline_queue();
}

void telegram_cancel_alerts(const char *chat_id)
{
    tg_send_cancel_alert(chat_id);
}
