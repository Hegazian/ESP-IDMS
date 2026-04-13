/**
 * telegram.c — Public API wrapper for the Telegram bot module.
 *
 * Re-exports functions from internal submodules for use by the rest of
 * the application. All heavy lifting is done in:
 *   tg_http.c   — HTTP transport
 *   tg_send.c   — Message sending, alerts, offline queue
 *   tg_parse.c  — JSON extraction
 *   tg_ui.c     — Report builders, keyboard definitions
 *   tg_bot.c    — Poll task, command routing, NVS persistence
 */

#include "telegram.h"
#include "tg_http.h"
#include "tg_send.h"
#include "tg_bot.h"
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
    return tg_broadcast_text(text);
}

esp_err_t telegram_send_ringing_alert(const char *chat_id, const char *alert_text)
{
    return tg_send_ring_alert(chat_id, alert_text);
}

esp_err_t telegram_heartbeat(void)
{
    int st = 0;
    esp_err_t err = tg_http_get("/getMe", NULL, 0, &st);
    if (err == ESP_OK && st >= 200 && st < 300) {
        ESP_LOGD(TAG, "Heartbeat OK");
    }
    return err;
}

void telegram_command_poll_start(void)
{
    tg_bot_start();
}

void telegram_flush_offline_queue(void)
{
    tg_flush_offline_queue();
}
