#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/**
 * tg_send.c — Message sending: text, broadcasts, ring alerts,
 *              offline queue, inline keyboard delivery.
 */

/** URL-encode src and append to dst (exposed from tg_http for reuse) */
void tg_urlenc_append(char *dst, size_t sz, const char *src);

/**
 * Send a plain text message to a specific chat.
 * Supports HTML parse mode.
 */
esp_err_t tg_send_text(const char *chat_id, const char *text);

/**
 * Send a message with an inline keyboard.
 * kb_json is a raw inline_keyboard JSON array string.
 */
esp_err_t tg_send_kb(const char *chat_id, const char *text, const char *kb_json);

/**
 * Acknowledge a callback query to stop the loading spinner.
 */
esp_err_t tg_answer_cb(const char *cb_id);

/**
 * Broadcast text to ALL registered technicians.
 */
esp_err_t tg_broadcast_text(const char *text);

/** Alias for tg_broadcast_text — for alert-style messages */
static inline esp_err_t tg_broadcast_alert(const char *text) {
    return tg_broadcast_text(text);
}

/**
 * Ringing alert: 5 rapid messages with per-chat cooldown.
 * Each message triggers a phone notification sound.
 */
esp_err_t tg_send_ring_alert(const char *chat_id, const char *alert_text);

/**
 * Flush any queued offline alerts now that connectivity is restored.
 */
void tg_flush_offline_queue(void);

/**
 * Queue a message for later delivery (when offline).
 * Returns ESP_OK if queued, ESP_ERR_NO_MEM if queue is full.
 */
esp_err_t tg_queue_message(const char *chat_id, const char *text);
