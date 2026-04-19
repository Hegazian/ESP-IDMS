#pragma once

#include <stdbool.h>
#include <stddef.h>

/**
 * tg_parse.c — Minimal JSON extraction for Telegram getUpdates responses.
 *
 * No library dependency — all functions use strstr + manual parsing.
 * All functions are safe for truncated / partial JSON fragments.
 * Caller must provide output buffers — no static internal buffers.
 */

/**
 * Extract a JSON value (string or number) for a given key prefix.
 * Handles "key":"value" and "key":123.
 * Returns pointer into out buffer, or NULL if not found.
 * out and out_sz must be valid (NULL/0 returns NULL).
 */
const char *tg_json_val(const char *json, const char *key, char *out, size_t out_sz);

/**
 * Extract callback_query.id from within the callback_query object.
 * This is the correct way — Telegram's callback_query.id is a string
 * that lives inside the callback_query object, NOT at the top level.
 */
const char *tg_json_cb_id(const char *json, char *out, size_t out_sz);

/**
 * Extract the message text field.
 * Caller must provide out buffer (recommended: 512 bytes).
 * Returns NULL if no text field exists.
 */
const char *tg_json_text(const char *json, char *out, size_t out_sz);

/** Check if the response contains a "message" object. */
bool tg_is_msg(const char *json);

/** Check if the response contains a "callback_query" object. */
bool tg_is_cb(const char *json);

/**
 * Check if the message text starts with "/cmd".
 * Single text extraction — O(n) total, not O(n * commands).
 */
bool tg_is_cmd(const char *json, const char *cmd);

/**
 * Check if the sender (from.id) is in the registered technician list.
 */
bool tg_is_authorized(const char *json);

/**
 * Extract the chat ID from a getUpdates response.
 * Tries message.chat.id and callback_query.message.chat.id.
 * Caller must provide out buffer (recommended: 32 bytes).
 */
const char *tg_json_chat(const char *json, char *out, size_t out_sz);
