#pragma once

#include <stddef.h>

/**
 * Get the Telegram bot token from NVS secrets (or Kconfig fallback).
 * Returns the token into out buffer. Returns empty string if not configured.
 */
void tg_get_token(char *out, size_t out_sz);