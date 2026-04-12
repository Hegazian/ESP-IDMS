#pragma once

#include "esp_err.h"

esp_err_t telegram_send_text(const char *chat_id, const char *text);
esp_err_t telegram_broadcast_text(const char *text);
esp_err_t telegram_heartbeat(void);
