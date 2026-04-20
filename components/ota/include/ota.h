#pragma once

#include "esp_err.h"
#include <stdbool.h>

/**
 * Initialize OTA subsystem.
 * - Validates current boot state (rollback check)
 * - Starts HTTP server for firmware upload if OTA enabled
 * - Registers Telegram command handler if enabled
 *
 * Call once after Wi-Fi is connected.
 */
esp_err_t ota_init(void);

/**
 * Get the currently running firmware version string.
 * Returns a pointer to a static buffer (do not free).
 * Format: "v<PROJECT_VER>-<chip>-<date>_<time>"
 */
const char *ota_get_version(void);

/**
 * Get human-readable OTA status.
 * Returns one of: "ready", "updating", "rollback pending", "rollback occurred"
 */
const char *ota_get_status(void);

/**
 * Get the current OTA partition label ("factory", "ota_0", "ota_1").
 */
const char *ota_get_partition(void);

/**
 * Signal that the new firmware is healthy after an OTA update.
 * Call this once all subsystems are confirmed operational.
 * Marks the running partition as valid (prevents rollback).
 */
void ota_mark_app_valid(void);

/**
 * Trigger an OTA update via Telegram command.
 * Sets a flag so the HTTP server knows to expect an upload soon.
 */
void ota_trigger_from_telegram(void);

/**
 * Check if an OTA update was requested via Telegram.
 * Returns true if /ota_update was sent, resets the flag.
 */
bool ota_is_requested(void);
