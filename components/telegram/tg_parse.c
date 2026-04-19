#include "tg_parse.h"
#include "config_store.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "tg_parse";

/* ------------------------------------------------------------------ */
/*  JSON value extraction                                              */
/* ------------------------------------------------------------------ */

const char *tg_json_val(const char *json, const char *key, char *out, size_t out_sz)
{
    if (!out || out_sz == 0) return NULL;

    const char *kp = strstr(json, key);
    if (!kp) return NULL;
    const char *vs = kp + strlen(key);
    while (*vs == ':' || *vs == ' ' || *vs == '\t') vs++;
    if (!*vs) return NULL;

    if (*vs == '"') {
        vs++;
        const char *end = strchr(vs, '"');
        if (!end) return NULL;
        size_t len = end - vs;
        if (len >= out_sz) len = out_sz - 1;
        memcpy(out, vs, len);
        out[len] = '\0';
    } else {
        size_t len = 0;
        while (vs[len] && vs[len] != ',' && vs[len] != '}' && vs[len] != ']' && len < out_sz - 1) {
            len++;
        }
        memcpy(out, vs, len);
        out[len] = '\0';
    }
    return out;
}

/**
 * Extract callback_query.id — must be found WITHIN the callback_query object.
 * Finds the opening { of the object, then searches for "id":" within it.
 */
const char *tg_json_cb_id(const char *json, char *out, size_t out_sz)
{
    if (!out || out_sz == 0) return NULL;

    const char *cq = strstr(json, "\"callback_query\":");
    if (!cq) return NULL;
    const char *brace = strchr(cq, '{');
    if (!brace) return NULL;

    /* Limit search to avoid matching nested objects */
    char tmp[512];
    const char *end = strchr(brace, '}');
    size_t max = end ? (size_t)(end - brace) : 500;
    if (max >= sizeof(tmp)) max = sizeof(tmp) - 1;
    memcpy(tmp, brace, max);
    tmp[max] = '\0';

    const char *kp = strstr(tmp, "\"id\":\"");
    if (!kp) return NULL;
    kp += 6;
    const char *e = strchr(kp, '"');
    if (!e) return NULL;
    size_t len = e - kp;
    if (len >= out_sz) len = out_sz - 1;
    memcpy(out, kp, len);
    out[len] = '\0';
    return out;
}

const char *tg_json_text(const char *json, char *out, size_t out_sz)
{
    const char *t = tg_json_val(json, "\"text\":\"", out, out_sz);
    if (t) return t;
    return tg_json_val(json, "\"text\": \"", out, out_sz);
}

bool tg_is_msg(const char *json)
{
    return strstr(json, "\"message\":{") || strstr(json, "\"message\": {");
}

bool tg_is_cb(const char *json)
{
    return strstr(json, "\"callback_query\":") != NULL;
}

bool tg_is_cmd(const char *json, const char *cmd)
{
    char text_buf[512];
    const char *text = tg_json_text(json, text_buf, sizeof(text_buf));
    if (!text || text[0] != '/') return false;
    text++; /* skip / */
    size_t cl = strlen(cmd);
    if (strncmp(text, cmd, cl) != 0) return false;
    char after = text[cl];
    return (after == '\0' || after == ' ' || after == '@' || after == '\n' || after == '\r');
}

bool tg_is_authorized(const char *json)
{
    char fid[32];

    /* For callback_query: the from.id is inside callback_query, NOT inside
     * the embedded message object (which belongs to the bot, not the user).
     * Search callback_query FIRST to avoid false matches. */
    const char *cq = strstr(json, "\"callback_query\":");
    if (cq) {
        const char *from = strstr(cq, "\"from\":");
        if (from) {
            const char *idp = strstr(from, "\"id\":");
            if (idp && tg_json_val(idp, "\"id\":", fid, sizeof(fid))) {
                goto check;
            }
        }
    }

    /* For plain messages: look for message.from.id */
    const char *m = strstr(json, "\"message\":");
    if (m) {
        const char *from = strstr(m, "\"from\":");
        if (from) {
            const char *idp = strstr(from, "\"id\":");
            if (idp && tg_json_val(idp, "\"id\":", fid, sizeof(fid))) {
                goto check;
            }
        }
    }

    ESP_LOGD(TAG, "Auth: no from.id found");
    return false;

check:
    ESP_LOGD(TAG, "Auth: from.id=%s", fid);
    uint8_t n = config_get_tech_count();
    ESP_LOGD(TAG, "Auth: %d techs registered", n);
    for (int i = 0; i < n; i++) {
        char tid[64];
        if (config_get_tech_id(i, tid, sizeof(tid)) == ESP_OK) {
            ESP_LOGD(TAG, "Auth: tech[%d]=%s", i, tid);
            if (strcmp(fid, tid) == 0)
                return true;
        }
    }
    ESP_LOGW(TAG, "Auth: %s not in tech list", fid);
    return false;
}

const char *tg_json_chat(const char *json, char *out, size_t out_sz)
{
    const char *t = tg_json_val(json, "\"chat\":{\"id\"", out, out_sz);
    if (t) return t;
    t = tg_json_val(json, "\"chat\": {\"id\"", out, out_sz);
    if (t) return t;

    /* callback_query.message.chat.id */
    const char *cq = strstr(json, "\"callback_query\":");
    if (cq) {
        const char *msg = strstr(cq, "\"message\":");
        if (msg) {
            const char *ch = strstr(msg, "\"chat\":{\"id\"");
            if (ch) return tg_json_val(ch, "\"chat\":{\"id\"", out, out_sz);
            ch = strstr(msg, "\"chat\": {\"id\"");
            if (ch) return tg_json_val(ch, "\"chat\": {\"id\"", out, out_sz);
        }
    }
    return NULL;
}
