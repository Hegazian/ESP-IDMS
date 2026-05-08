#pragma once

#include <stddef.h>

#define TG_KB_MAIN \
"[" \
"[{\"text\":\"Status\",\"callback_data\":\"cmd_status\"}," \
" {\"text\":\"Test Alert\",\"callback_data\":\"cmd_test\"}]," \
"[{\"text\":\"Weekly Report\",\"callback_data\":\"cmd_weekly\"}," \
" {\"text\":\"Export CSV\",\"callback_data\":\"cmd_export\"}]," \
"[{\"text\":\"OTA Update\",\"callback_data\":\"cmd_ota\"}," \
" {\"text\":\"Reboot\",\"callback_data\":\"cmd_reboot\"}]," \
"[{\"text\":\"Technician IDs\",\"callback_data\":\"cmd_techs\"}]" \
"]"

#define TG_KB_OTA \
"[" \
"[{\"text\":\"OTA Status\",\"callback_data\":\"ota_status\"}," \
" {\"text\":\"Show URL\",\"callback_data\":\"ota_url\"}]," \
"[{\"text\":\"Back\",\"callback_data\":\"back_main\"}]" \
"]"

#define TG_KB_TECHS \
"[" \
"[{\"text\":\"List IDs\",\"callback_data\":\"tech_list\"}," \
" {\"text\":\"Remove ID\",\"callback_data\":\"tech_remove\"}]," \
"[{\"text\":\"Back\",\"callback_data\":\"back_main\"}]" \
"]"

#define TG_KB_REBOOT_CONFIRM \
"[" \
"[{\"text\":\"Yes, Reboot\",\"callback_data\":\"confirm_reboot\"}," \
" {\"text\":\"Cancel\",\"callback_data\":\"cancel_reboot\"}]" \
"]"

void tg_build_status(char *buf, size_t sz);

void tg_build_weekly(char *buf, size_t sz);

void tg_build_export(char *buf, size_t sz);

void tg_build_ota(char *buf, size_t sz);

void tg_build_techs(char *buf, size_t sz);

void tg_build_reboot_confirm(char *buf, size_t sz);

void tg_build_tech_remove(char *buf, size_t sz);
