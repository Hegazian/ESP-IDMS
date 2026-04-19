#pragma once

#include <stddef.h>

/**
 * tg_ui.c — Report builders and inline keyboard definitions.
 *
 * Produces formatted text buffers for status, OTA, and technician reports.
 * Provides keyboard JSON macros for the bot menu system.
 */

/* Inline keyboard JSON definitions — use directly in send_kb() calls */
#define TG_KB_MAIN \
"[" \
"[{\"text\":\"📊 Status\",\"callback_data\":\"cmd_status\"}," \
" {\"text\":\"🔔 Test Alert\",\"callback_data\":\"cmd_test\"}]," \
"[{\"text\":\"🔄 OTA Update\",\"callback_data\":\"cmd_ota\"}," \
" {\"text\":\"🔧 Reboot\",\"callback_data\":\"cmd_reboot\"}]," \
"[{\"text\":\"📋 Technician IDs\",\"callback_data\":\"cmd_techs\"}]" \
"]"

#define TG_KB_OTA \
"[" \
"[{\"text\":\"📡 OTA Status\",\"callback_data\":\"ota_status\"}," \
" {\"text\":\"🌐 Show URL\",\"callback_data\":\"ota_url\"}]," \
"[{\"text\":\"◀️ Back\",\"callback_data\":\"back_main\"}]" \
"]"

#define TG_KB_TECHS \
"[" \
"[{\"text\":\"📋 List IDs\",\"callback_data\":\"tech_list\"}]," \
"[{\"text\":\"◀️ Back\",\"callback_data\":\"back_main\"}]" \
"]"

/**
 * Build a status report into buf.
 */
void tg_build_status(char *buf, size_t sz);

/**
 * Build an OTA report into buf.
 */
void tg_build_ota(char *buf, size_t sz);

/**
 * Build a technician list report into buf.
 */
void tg_build_techs(char *buf, size_t sz);
