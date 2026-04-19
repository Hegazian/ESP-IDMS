#include "tg_send.h"
#include "tg_http.h"
#include "config_store.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "tg_send";

/* ------------------------------------------------------------------ */
/*  Constants                                                          */
/* ------------------------------------------------------------------ */

#define RING_COOLDOWN_MS        60000
#define MAX_RING_COOLDOWNS      8
#define MAX_OFFLINE_QUEUE       8

/* ------------------------------------------------------------------ */
/*  State                                                              */
/* ------------------------------------------------------------------ */

/* Per-chat ring cooldown: hash table */
static struct { uint32_t id; uint32_t tick; } s_ring_cd[MAX_RING_COOLDOWNS];

/* Offline alert queue */
static struct {
    char chat_id[32];
    char text[256];
} s_offline_q[MAX_OFFLINE_QUEUE];
static int s_offline_cnt = 0;

/* ------------------------------------------------------------------ */
/*  Send primitives                                                      */
/* ------------------------------------------------------------------ */

esp_err_t tg_send_text(const char *chat_id, const char *text)
{
    if (!chat_id || !text) return ESP_ERR_INVALID_ARG;
    char body[1024];
    snprintf(body, sizeof(body), "chat_id=%s&parse_mode=HTML&text=", chat_id);
    tg_urlenc_append(body, sizeof(body), text);
    return tg_http_post_form("/sendMessage", body);
}

esp_err_t tg_send_kb(const char *chat_id, const char *text, const char *kb)
{
    if (!chat_id || !text) return ESP_ERR_INVALID_ARG;
    char enc[1024]; enc[0] = '\0';
    tg_urlenc_append(enc, sizeof(enc), text);
    char body[2048];
    snprintf(body, sizeof(body),
             "chat_id=%s&parse_mode=HTML&text=%s&reply_markup={\"inline_keyboard\":%s}",
             chat_id, enc, kb);
    return tg_http_post_form("/sendMessage", body);
}

esp_err_t tg_answer_cb(const char *cb_id)
{
    char body[128];
    snprintf(body, sizeof(body), "callback_query_id=%s&show_alert=false", cb_id);
    return tg_http_post_form("/answerCallbackQuery", body);
}

/* ------------------------------------------------------------------ */
/*  Broadcast                                                          */
/* ------------------------------------------------------------------ */

esp_err_t tg_broadcast_text(const char *text)
{
    uint8_t n = config_get_tech_count();
    if (n == 0) { ESP_LOGW(TAG, "No technicians configured"); return ESP_ERR_NOT_FOUND; }
    esp_err_t last = ESP_OK;
    for (int i = 0; i < n; i++) {
        char id[64];
        if (config_get_tech_id(i, id, sizeof(id)) == ESP_OK) {
            esp_err_t e = tg_send_text(id, text);
            if (e != ESP_OK) last = e;
        }
    }
    return last;
}

/* ------------------------------------------------------------------ */
/*  Ringing alert with per-chat cooldown                               */
/* ------------------------------------------------------------------ */

static uint32_t ring_hash(const char *s)
{
    uint32_t h = 5381;
    for (const char *p = s; *p; p++)
        h = ((h << 5) + h) + (unsigned char)*p;
    return h;
}

static bool ring_cooldown(const char *chat_id)
{
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    uint32_t hash = ring_hash(chat_id);

    for (int i = 0; i < MAX_RING_COOLDOWNS; i++) {
        if (s_ring_cd[i].id == hash) {
            if ((now - s_ring_cd[i].tick) < RING_COOLDOWN_MS)
                return true;
            s_ring_cd[i].tick = now;
            return false;
        }
    }
    for (int i = 0; i < MAX_RING_COOLDOWNS; i++) {
        if (s_ring_cd[i].id == 0) {
            s_ring_cd[i].id = hash;
            s_ring_cd[i].tick = now;
            return false;
        }
    }
    /* Evict oldest */
    uint32_t oi = 0, ot = s_ring_cd[0].tick;
    for (int i = 1; i < MAX_RING_COOLDOWNS; i++) {
        if (s_ring_cd[i].tick < ot) { oi = i; ot = s_ring_cd[i].tick; }
    }
    s_ring_cd[oi].id = hash;
    s_ring_cd[oi].tick = now;
    return false;
}

esp_err_t tg_send_ring_alert(const char *chat_id, const char *alert_text)
{
    if (!chat_id || !alert_text) return ESP_ERR_INVALID_ARG;
    if (ring_cooldown(chat_id)) {
        ESP_LOGD(TAG, "Ring cooldown active for %s", chat_id);
        return ESP_OK;
    }

    const char *rings[] = {
        "📞📞📞📞📞",
        "🔔 <b>INCOMING ALERT</b> 🔔\nESP-IDMS has detected a fault!",
        "⚠️ <b>DEVICE ALERT</b> ⚠️\nPlease check the device immediately!",
        "🚨 <b>FAULT DETECTED</b> 🚨\nAttention required now!",
        "📡 <b>ESP-IDMS ALERT</b> 📡\nFault condition requires immediate attention!"
    };
    for (int i = 0; i < 5; i++) {
        esp_err_t e = tg_send_text(chat_id, rings[i]);
        if (e != ESP_OK) {
            tg_queue_message(chat_id, rings[i]);
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    tg_send_text(chat_id, alert_text);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Offline queue                                                      */
/* ------------------------------------------------------------------ */

esp_err_t tg_queue_message(const char *chat_id, const char *text)
{
    if (!chat_id || !text || s_offline_cnt >= MAX_OFFLINE_QUEUE)
        return ESP_ERR_NO_MEM;
    strncpy(s_offline_q[s_offline_cnt].chat_id, chat_id, 31);
    s_offline_q[s_offline_cnt].chat_id[31] = '\0';
    strncpy(s_offline_q[s_offline_cnt].text, text, 255);
    s_offline_q[s_offline_cnt].text[255] = '\0';
    s_offline_cnt++;
    return ESP_OK;
}

void tg_flush_offline_queue(void)
{
    if (s_offline_cnt == 0) return;
    ESP_LOGI(TAG, "Flushing %d queued alerts", s_offline_cnt);
    int failed = 0;
    for (int i = 0; i < s_offline_cnt; i++) {
        if (tg_send_text(s_offline_q[i].chat_id, s_offline_q[i].text) != ESP_OK)
            failed++;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (failed == 0) {
        s_offline_cnt = 0;
        ESP_LOGI(TAG, "Queue flushed OK");
    } else {
        ESP_LOGW(TAG, "Flush had %d failures — entries retained", failed);
    }
}
