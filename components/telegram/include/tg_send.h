#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

void tg_urlenc_append(char *dst, size_t sz, const char *src);

esp_err_t tg_send_text(const char *chat_id, const char *text);

esp_err_t tg_send_text_ext(const char *chat_id, const char *text, bool disable_preview);

esp_err_t tg_send_kb(const char *chat_id, const char *text, const char *kb_json);

esp_err_t tg_answer_cb(const char *cb_id);

esp_err_t tg_broadcast_text(const char *text);

esp_err_t tg_broadcast_alert(const char *text);

esp_err_t tg_send_ring_alert(const char *chat_id, const char *alert_text);

void tg_flush_offline_queue(void);

esp_err_t tg_queue_message(const char *chat_id, const char *text);

void tg_send_init(void);

void tg_send_process_reminders(void);

void tg_send_cancel_alert(const char *chat_id);
