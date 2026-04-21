#include "tg_send.h"
#include "tg_http.h"
#include "config_store.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "tg_send";

#define RING_COOLDOWN_MS        60000
#define MAX_RING_COOLDOWNS      8
#define MAX_OFFLINE_QUEUE       8
#define RING_REMIND_INTERVAL_S  120
#define RING_MAX_REMINDERS      3

static struct { uint32_t id; uint32_t tick; } s_ring_cd[MAX_RING_COOLDOWNS];

static struct {
    char chat_id[32];
    char text[256];
    uint32_t last_ring_tick;
    uint8_t reminders_sent;
    bool active;
} s_ring_remind[8];

static int s_ring_remind_count = 0;

static struct {
    char chat_id[32];
    char text[512];
} s_offline_q[MAX_OFFLINE_QUEUE];
static int s_offline_cnt = 0;
static SemaphoreHandle_t s_queue_mux = NULL;

void tg_send_init(void)
{
    s_queue_mux = xSemaphoreCreateMutex();
}

esp_err_t tg_send_text(const char *chat_id, const char *text)
{
    return tg_send_text_ext(chat_id, text, false);
}

esp_err_t tg_send_text_ext(const char *chat_id, const char *text, bool disable_preview)
{
    if (!chat_id || !text) return ESP_ERR_INVALID_ARG;
    char body[2048];
    snprintf(body, sizeof(body), "chat_id=%s&parse_mode=HTML&text=", chat_id);
    tg_urlenc_append(body, sizeof(body), text);
    if (disable_preview) {
        size_t len = strlen(body);
        snprintf(body + len, sizeof(body) - len, "&disable_web_page_preview=true");
    }
    return tg_http_post_form("/sendMessage", body);
}

esp_err_t tg_send_kb(const char *chat_id, const char *text, const char *kb)
{
    if (!chat_id || !text) return ESP_ERR_INVALID_ARG;
    char enc[2048]; enc[0] = '\0';
    tg_urlenc_append(enc, sizeof(enc), text);
    char reply_markup[512];
    snprintf(reply_markup, sizeof(reply_markup), "{\"inline_keyboard\":%s}", kb);
    char rm_enc[1024]; rm_enc[0] = '\0';
    tg_urlenc_append(rm_enc, sizeof(rm_enc), reply_markup);
    int body_sz = 128 + (int)strlen(chat_id) + (int)strlen(enc) + (int)strlen(rm_enc);
    char *body = malloc(body_sz);
    if (!body) return ESP_ERR_NO_MEM;
    snprintf(body, body_sz,
             "chat_id=%s&parse_mode=HTML&text=%s&reply_markup=%s",
             chat_id, enc, rm_enc);
    esp_err_t ret = tg_http_post_form("/sendMessage", body);
    free(body);
    return ret;
}

esp_err_t tg_answer_cb(const char *cb_id)
{
    char body[256];
    snprintf(body, sizeof(body), "callback_query_id=%s&show_alert=false", cb_id);
    return tg_http_post_form("/answerCallbackQuery", body);
}

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

esp_err_t tg_broadcast_alert(const char *text)
{
    uint8_t n = config_get_tech_count();
    if (n == 0) { ESP_LOGW(TAG, "No technicians configured"); return ESP_ERR_NOT_FOUND; }
    esp_err_t last = ESP_OK;
    for (int i = 0; i < n; i++) {
        char id[64];
        if (config_get_tech_id(i, id, sizeof(id)) == ESP_OK) {
            esp_err_t e = tg_send_text_ext(id, text, true);
            if (e != ESP_OK) last = e;
        }
    }
    return last;
}

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

    uint32_t now_s = xTaskGetTickCount() * portTICK_PERIOD_MS / 1000;

    const char *burst1[] = {
        "\xf0\x9f\x93\x9e\xf0\x9f\x93\x9e\xf0\x9f\x93\x9e",
        "\xf0\x9f\x94\x94 <b>INCOMING ALERT</b> \xf0\x9f\x94\x94",
        "\xf0\x9f\x93\x9e\xf0\x9f\x93\x9e\xf0\x9f\x93\x9e",
        "\xf0\x9f\x94\x94 <b>INCOMING ALERT</b> \xf0\x9f\x94\x94",
        "\xf0\x9f\x93\x9e\xf0\x9f\x93\x9e\xf0\x9f\x93\x9e",
    };
    for (int i = 0; i < 5; i++) {
        tg_send_text(chat_id, burst1[i]);
        vTaskDelay(pdMS_TO_TICKS(150));
    }

    vTaskDelay(pdMS_TO_TICKS(800));

    const char *burst2[] = {
        "\xf0\x9f\x9a\xa8 <b>FAULT DETECTED</b> \xf0\x9f\x9a\xa8",
        "\xe2\x9a\xa0\xef\xb8\x8f <b>DEVICE ALERT</b> \xe2\x9a\xa0\xef\xb8\x8f",
        "\xf0\x9f\x9a\xa8 <b>FAULT DETECTED</b> \xf0\x9f\x9a\xa8",
        "\xe2\x9a\xa0\xef\xb8\x8f <b>DEVICE ALERT</b> \xe2\x9a\xa0\xef\xb8\x8f",
    };
    for (int i = 0; i < 4; i++) {
        tg_send_text(chat_id, burst2[i]);
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    vTaskDelay(pdMS_TO_TICKS(600));

    tg_send_text_ext(chat_id, alert_text, true);

    vTaskDelay(pdMS_TO_TICKS(400));

    tg_send_text(chat_id, "\xf0\x9f\x94\xb4 <b>ALERT ACTIVE</b> \xf0\x9f\x94\xb4\nThis alert will repeat until acknowledged.");

    for (int i = 0; i < s_ring_remind_count; i++) {
        if (s_ring_remind[i].active && strcmp(s_ring_remind[i].chat_id, chat_id) == 0) {
            s_ring_remind[i].last_ring_tick = now_s;
            s_ring_remind[i].reminders_sent = 0;
            strncpy(s_ring_remind[i].text, alert_text, sizeof(s_ring_remind[i].text) - 1);
            s_ring_remind[i].text[sizeof(s_ring_remind[i].text) - 1] = '\0';
            return ESP_OK;
        }
    }
    if (s_ring_remind_count < 8) {
        int idx = s_ring_remind_count;
        strncpy(s_ring_remind[idx].chat_id, chat_id, sizeof(s_ring_remind[idx].chat_id) - 1);
        s_ring_remind[idx].chat_id[sizeof(s_ring_remind[idx].chat_id) - 1] = '\0';
        strncpy(s_ring_remind[idx].text, alert_text, sizeof(s_ring_remind[idx].text) - 1);
        s_ring_remind[idx].text[sizeof(s_ring_remind[idx].text) - 1] = '\0';
        s_ring_remind[idx].last_ring_tick = now_s;
        s_ring_remind[idx].reminders_sent = 0;
        s_ring_remind[idx].active = true;
        s_ring_remind_count++;
    }

    return ESP_OK;
}

void tg_send_process_reminders(void)
{
    uint32_t now_s = xTaskGetTickCount() * portTICK_PERIOD_MS / 1000;

    for (int i = 0; i < s_ring_remind_count; i++) {
        if (!s_ring_remind[i].active) continue;

        uint32_t elapsed = now_s - s_ring_remind[i].last_ring_tick;
        if (elapsed < (uint32_t)RING_REMIND_INTERVAL_S) continue;
        if (s_ring_remind[i].reminders_sent >= RING_MAX_REMINDERS) continue;

        s_ring_remind[i].reminders_sent++;
        s_ring_remind[i].last_ring_tick = now_s;

        ESP_LOGI(TAG, "Alert reminder %d/%d for %s",
                 s_ring_remind[i].reminders_sent, RING_MAX_REMINDERS,
                 s_ring_remind[i].chat_id);

        const char *remind_burst[] = {
            "\xf0\x9f\x93\xb2 \xf0\x9f\x94\xb4 <b>ALERT STILL ACTIVE</b> \xf0\x9f\x94\xb4 \xf0\x9f\x93\xb2",
            "\xf0\x9f\x9a\xa8 <b>UNACKNOWLEDGED ALERT</b> \xf0\x9f\x9a\xa8",
            "\xf0\x9f\x93\x9e\xf0\x9f\x93\x9e\xf0\x9f\x93\x9e\xf0\x9f\x93\x9e\xf0\x9f\x93\x9e",
        };
        for (int j = 0; j < 3; j++) {
            tg_send_text(s_ring_remind[i].chat_id, remind_burst[j]);
            vTaskDelay(pdMS_TO_TICKS(200));
        }

        vTaskDelay(pdMS_TO_TICKS(500));
        tg_send_text_ext(s_ring_remind[i].chat_id, s_ring_remind[i].text, true);

        char footer[128];
        snprintf(footer, sizeof(footer),
                 "\n\xf0\x9f\x94\x81 Reminder %d of %d. Send /status to check.",
                 s_ring_remind[i].reminders_sent, RING_MAX_REMINDERS);
        tg_send_text(s_ring_remind[i].chat_id, footer);
    }
}

void tg_send_cancel_alert(const char *chat_id)
{
    for (int i = 0; i < s_ring_remind_count; i++) {
        if (s_ring_remind[i].active && strcmp(s_ring_remind[i].chat_id, chat_id) == 0) {
            s_ring_remind[i].active = false;
            ESP_LOGI(TAG, "Alert reminders cancelled for %s", chat_id);
        }
    }
}

esp_err_t tg_queue_message(const char *chat_id, const char *text)
{
    if (!chat_id || !text) return ESP_ERR_INVALID_ARG;
    if (!s_queue_mux) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_queue_mux, pdMS_TO_TICKS(500)) != pdTRUE)
        return ESP_ERR_TIMEOUT;

    esp_err_t result = ESP_ERR_NO_MEM;
    if (s_offline_cnt < MAX_OFFLINE_QUEUE) {
        strncpy(s_offline_q[s_offline_cnt].chat_id, chat_id, 31);
        s_offline_q[s_offline_cnt].chat_id[31] = '\0';
        strncpy(s_offline_q[s_offline_cnt].text, text, 511);
        s_offline_q[s_offline_cnt].text[511] = '\0';
        s_offline_cnt++;
        result = ESP_OK;
    }
    xSemaphoreGive(s_queue_mux);
    return result;
}

void tg_flush_offline_queue(void)
{
    if (!s_queue_mux) return;
    if (xSemaphoreTake(s_queue_mux, pdMS_TO_TICKS(1000)) != pdTRUE) return;

    if (s_offline_cnt == 0) {
        xSemaphoreGive(s_queue_mux);
        return;
    }
    int cnt = s_offline_cnt;
    xSemaphoreGive(s_queue_mux);

    ESP_LOGI(TAG, "Flushing %d queued alerts", cnt);
    int failed = 0;
    for (int i = 0; i < cnt; i++) {
        if (tg_send_text(s_offline_q[i].chat_id, s_offline_q[i].text) != ESP_OK)
            failed++;
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (xSemaphoreTake(s_queue_mux, pdMS_TO_TICKS(1000)) == pdTRUE) {
        if (failed == 0) {
            s_offline_cnt = 0;
            ESP_LOGI(TAG, "Queue flushed OK");
        } else {
            ESP_LOGW(TAG, "Flush had %d failures — entries retained", failed);
        }
        xSemaphoreGive(s_queue_mux);
    }
}
