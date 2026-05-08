#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "cJSON.h"

typedef struct {
    int update_id;
    bool is_message;
    bool is_callback;
    char from_id[32];
    char chat_id[32];
    char message_text[512];
    char callback_id[64];
    char callback_data[64];
} tg_update_t;

bool tg_parse_update(const char *json_response, tg_update_t *out);

bool tg_parse_first_update_id(const char *json_response, int *update_id);

bool tg_is_authorized_id(const char *from_id);

bool tg_is_cmd_text(const char *text, const char *cmd);
