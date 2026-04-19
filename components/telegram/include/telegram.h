#pragma once

#include "esp_err.h"

/**
 * Send a plain text message to a specific chat.
 */
esp_err_t telegram_send_text(const char *chat_id, const char *text);

/**
 * Broadcast a text message to ALL registered technicians.
 */
esp_err_t telegram_broadcast_text(const char *text);

/**
 * Broadcast a text alert message.
 * Each message triggers a notification sound on the recipient's phone
 * unless the user has muted the bot.
 */
esp_err_t telegram_broadcast_alert(const char *text);

/**
 * Send a "ringing" style alert — bursts 5 rapid messages to trigger
 * multiple notification sounds on the recipient's phone, simulating
 * a phone call ring.
 */
esp_err_t telegram_send_ringing_alert(const char *chat_id, const char *alert_text);

/**
 * Send a heartbeat (connectivity check) via getMe.
 */
esp_err_t telegram_heartbeat(void);

/**
 * Start the interactive bot poll task with inline keyboard menus.
 */
void telegram_command_poll_start(void);

/**
 * Flush any queued offline alerts now that connectivity is restored.
 */
void telegram_flush_offline_queue(void);
