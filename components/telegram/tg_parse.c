#include "tg_parse.h"
#include "config_store.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "tg_parse";

static void safe_strcpy(char *dst, size_t dst_sz, const char *src)
{
    if (!src) { dst[0] = '\0'; return; }
    strncpy(dst, src, dst_sz - 1);
    dst[dst_sz - 1] = '\0';
}

bool tg_parse_update(const char *json_response, tg_update_t *out)
{
    if (!json_response || !out) return false;
    memset(out, 0, sizeof(*out));

    cJSON *root = cJSON_Parse(json_response);
    if (!root) {
        ESP_LOGW(TAG, "JSON parse failed (response len=%d)", (int)strlen(json_response));
        return false;
    }

    cJSON *ok = cJSON_GetObjectItem(root, "ok");
    if (!cJSON_IsTrue(ok)) {
        cJSON *err_code = cJSON_GetObjectItem(root, "error_code");
        cJSON *desc = cJSON_GetObjectItem(root, "description");
        ESP_LOGW(TAG, "API error: %d %s",
                 err_code ? err_code->valueint : 0,
                 cJSON_IsString(desc) ? desc->valuestring : "");
        cJSON_Delete(root);
        return false;
    }

    cJSON *result = cJSON_GetObjectItem(root, "result");
    if (!cJSON_IsArray(result)) {
        ESP_LOGW(TAG, "No result array in response");
        cJSON_Delete(root);
        return false;
    }

    int arr_size = cJSON_GetArraySize(result);
    if (arr_size == 0) {
        cJSON_Delete(root);
        return false;
    }

    cJSON *update = cJSON_GetArrayItem(result, 0);
    if (!update) { cJSON_Delete(root); return false; }

    cJSON *uid = cJSON_GetObjectItem(update, "update_id");
    if (cJSON_IsNumber(uid)) {
        out->update_id = uid->valueint;
    }

    cJSON *msg = cJSON_GetObjectItem(update, "message");
    cJSON *cb = cJSON_GetObjectItem(update, "callback_query");

    if (msg) {
        out->is_message = true;
        cJSON *from = cJSON_GetObjectItem(msg, "from");
        if (from) {
            cJSON *fid = cJSON_GetObjectItem(from, "id");
            if (fid) {
                if (cJSON_IsString(fid)) {
                    safe_strcpy(out->from_id, sizeof(out->from_id), fid->valuestring);
                } else if (cJSON_IsNumber(fid)) {
                    snprintf(out->from_id, sizeof(out->from_id), "%lld", (long long)fid->valuedouble);
                }
            }
        }
        cJSON *chat = cJSON_GetObjectItem(msg, "chat");
        if (chat) {
            cJSON *cid = cJSON_GetObjectItem(chat, "id");
            if (cid) {
                if (cJSON_IsString(cid)) {
                    safe_strcpy(out->chat_id, sizeof(out->chat_id), cid->valuestring);
                } else if (cJSON_IsNumber(cid)) {
                    snprintf(out->chat_id, sizeof(out->chat_id), "%lld", (long long)cid->valuedouble);
                }
            }
        }
        cJSON *text = cJSON_GetObjectItem(msg, "text");
        if (cJSON_IsString(text)) {
            safe_strcpy(out->message_text, sizeof(out->message_text), text->valuestring);
        }
        cJSON_Delete(root);
        return true;
    } else if (cb) {
        out->is_callback = true;
        cJSON *from = cJSON_GetObjectItem(cb, "from");
        if (from) {
            cJSON *fid = cJSON_GetObjectItem(from, "id");
            if (fid) {
                if (cJSON_IsString(fid)) {
                    safe_strcpy(out->from_id, sizeof(out->from_id), fid->valuestring);
                } else if (cJSON_IsNumber(fid)) {
                    snprintf(out->from_id, sizeof(out->from_id), "%lld", (long long)fid->valuedouble);
                }
            }
        }
        cJSON *cb_msg = cJSON_GetObjectItem(cb, "message");
        if (cb_msg) {
            cJSON *chat = cJSON_GetObjectItem(cb_msg, "chat");
            if (chat) {
                cJSON *cid = cJSON_GetObjectItem(chat, "id");
                if (cid) {
                    if (cJSON_IsString(cid)) {
                        safe_strcpy(out->chat_id, sizeof(out->chat_id), cid->valuestring);
                    } else if (cJSON_IsNumber(cid)) {
                        snprintf(out->chat_id, sizeof(out->chat_id), "%lld", (long long)cid->valuedouble);
                    }
                }
            }
        }
        cJSON *cbid = cJSON_GetObjectItem(cb, "id");
        if (cJSON_IsString(cbid)) {
            safe_strcpy(out->callback_id, sizeof(out->callback_id), cbid->valuestring);
        }
        cJSON *cbdata = cJSON_GetObjectItem(cb, "data");
        if (cJSON_IsString(cbdata)) {
            safe_strcpy(out->callback_data, sizeof(out->callback_data), cbdata->valuestring);
        }
        cJSON_Delete(root);
        return true;
    }

    ESP_LOGW(TAG, "Update %d has no message or callback_query", out->update_id);
    cJSON_Delete(root);
    return false;
}

bool tg_is_authorized_id(const char *from_id)
{
    if (!from_id || from_id[0] == '\0') return false;
    uint8_t n = config_get_tech_count();
    for (int i = 0; i < n; i++) {
        char tid[64];
        if (config_get_tech_id(i, tid, sizeof(tid)) == ESP_OK) {
            if (strcmp(from_id, tid) == 0) return true;
        }
    }
    return false;
}

bool tg_is_cmd_text(const char *text, const char *cmd)
{
    if (!text || text[0] != '/') return false;
    text++;
    size_t cl = strlen(cmd);
    if (strncmp(text, cmd, cl) != 0) return false;
    char after = text[cl];
    return (after == '\0' || after == ' ' || after == '@' || after == '\n' || after == '\r');
}
