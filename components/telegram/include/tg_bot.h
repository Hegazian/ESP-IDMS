#pragma once

#include "esp_err.h"
#include <stdbool.h>

/**
 * tg_bot.c — Interactive bot poll task.
 *
 * Handles:
 *  - getUpdates polling with adaptive backoff
 *  - DNS connectivity check
 *  - Command routing and callback dispatch
 *  - NVS-persisted update offset
 *  - Bot command registration
 *
 * Entry point: tg_bot_start()
 */

/**
 * Start the bot poll task.
 * Call once after Wi-Fi is connected.
 */
esp_err_t tg_bot_start(void);

/**
 * Check if DNS is reachable (called internally, exposed for diagnostics).
 */
bool tg_check_dns(void);
