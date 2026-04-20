#pragma once

#include "esp_err.h"

/**
 * tg_http.c — HTTP transport layer for Telegram Bot API.
 *
 * Handles URL-encoding, HTTP client creation, and form/JSON/GET helpers.
 * All functions are internal to the telegram module (static linkage).
 */

/* Initialize HTTP subsystem — called once by telegram bot task */
void tg_http_init(void);

/**
 * POST a URL-encoded form to a Telegram API path.
 * path example: "/sendMessage"
 */
esp_err_t tg_http_post_form(const char *path, const char *body);

/**
 * POST a JSON body to a Telegram API path.
 */
esp_err_t tg_http_post_json(const char *path, const char *body);

/**
 * GET a Telegram API path, capturing the response body into buf.
 */
esp_err_t tg_http_get(const char *path, char *buf, size_t buf_sz, int *status_out);
