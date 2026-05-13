#include "tg_http.h"
#include "tg_token.h"
#include "sdkconfig.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "tg_http";

#define TG_HTTP_TIMEOUT_MS 10000

/* ------------------------------------------------------------------ */
/*  URL-encoding                                                       */
/* ------------------------------------------------------------------ */

static size_t urlenc_char(char *dst, size_t left, char c)
{
    if (left < 4) return 0;
    const char *hex = "0123456789ABCDEF";
    unsigned char u = (unsigned char)c;
    if ((u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') ||
        u == '-' || u == '_' || u == '.' || u == '~') {
        dst[0] = (char)u; return 1;
    }
    if (u == ' ') { dst[0] = '+'; return 1; }
    dst[0] = '%'; dst[1] = hex[(u >> 4) & 0xF]; dst[2] = hex[u & 0xF];
    return 3;
}

void tg_urlenc_append(char *dst, size_t sz, const char *src)
{
    size_t used = strnlen(dst, sz);
    for (size_t i = 0; src[i] && used + 4 < sz; i++) {
        size_t n = urlenc_char(dst + used, sz - used, src[i]);
        if (n == 0) break;
        used += n;
        dst[used] = '\0';
    }
}

/* ------------------------------------------------------------------ */
/*  HTTP helpers                                                       */
/* ------------------------------------------------------------------ */

typedef struct { char *buf; size_t max; size_t got; bool overflow; } resp_ctx_t;

static esp_err_t http_event(esp_http_client_event_t *evt)
{
    resp_ctx_t *ctx = (resp_ctx_t *)evt->user_data;
    if (!ctx || !ctx->buf) return ESP_OK;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (ctx->got + evt->data_len < ctx->max) {
            memcpy(ctx->buf + ctx->got, evt->data, evt->data_len);
            ctx->got += evt->data_len;
            ctx->buf[ctx->got] = '\0';
        } else {
            ctx->overflow = true;
        }
    }
    return ESP_OK;
}

void tg_http_init(void)
{
    /* Nothing to init — stateless helpers */
}

static esp_http_client_handle_t http_new(const char *path, int method,
                                          const char *ctype, resp_ctx_t *r, const char *post)
{
    char token[128];
    tg_get_token(token, sizeof(token));
    if (token[0] == '\0') return NULL;
    char url[256];
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s%s",
             token, path);

    esp_http_client_config_t cfg = {
        .url = url, .method = method,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 2048,
        .timeout_ms = TG_HTTP_TIMEOUT_MS,
        .event_handler = r ? http_event : NULL,
        .user_data = r,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return NULL;
    if (ctype) esp_http_client_set_header(c, "Content-Type", ctype);
    if (post) esp_http_client_set_post_field(c, post, (int)strlen(post));
    return c;
}

esp_err_t tg_http_post_form(const char *path, const char *body)
{
    esp_http_client_handle_t c = http_new(path, HTTP_METHOD_POST,
                                           "application/x-www-form-urlencoded", NULL, body);
    if (!c) return ESP_FAIL;
    esp_err_t err = esp_http_client_perform(c);
    int st = esp_http_client_get_status_code(c);
    if (err == ESP_OK && (st < 200 || st >= 300)) {
        if (st == 404) {
            ESP_LOGE(TAG, "POST %s → 404 (bot token is invalid — set correct token via serial: set_token <token>)", path);
        } else if (st == 401) {
            ESP_LOGE(TAG, "POST %s → 401 (bot token unauthorized)", path);
        } else {
            ESP_LOGW(TAG, "POST %s → %d", path, st);
        }
        err = ESP_FAIL;
    }
    esp_http_client_cleanup(c);
    return err;
}

esp_err_t tg_http_post_json(const char *path, const char *body)
{
    esp_http_client_handle_t c = http_new(path, HTTP_METHOD_POST,
                                           "application/json", NULL, body);
    if (!c) return ESP_FAIL;
    esp_err_t err = esp_http_client_perform(c);
    int st = esp_http_client_get_status_code(c);
    if (err == ESP_OK && (st < 200 || st >= 300)) {
        if (st == 404) {
            ESP_LOGE(TAG, "POST %s → 404 (bot token is invalid — set correct token via serial: set_token <token>)", path);
        } else if (st == 401) {
            ESP_LOGE(TAG, "POST %s → 401 (bot token unauthorized)", path);
        } else {
            ESP_LOGW(TAG, "POST %s → %d", path, st);
        }
        err = ESP_FAIL;
    }
    esp_http_client_cleanup(c);
    return err;
}

esp_err_t tg_http_get(const char *path, char *buf, size_t sz, int *st_out)
{
    if (buf) buf[0] = '\0';
    resp_ctx_t ctx = { .buf = buf, .max = sz - 1, .got = 0, .overflow = false };
    esp_http_client_handle_t c = http_new(path, HTTP_METHOD_GET, NULL, &ctx, NULL);
    if (!c) {
        ESP_LOGE(TAG, "GET %s — failed to create HTTP client (token empty?)", path);
        if (st_out) *st_out = 0;
        return ESP_FAIL;
    }
    esp_err_t err = esp_http_client_perform(c);
    int st = esp_http_client_get_status_code(c);
    int content_len = esp_http_client_get_content_length(c);
    if (st_out) *st_out = st;
    if (ctx.overflow) {
        ESP_LOGW(TAG, "GET %s: response truncated (%d/%d bytes, buffer %zu) — increase buffer size",
                 path, (int)ctx.got, content_len, sz);
    }
    if (st == 404) {
        ESP_LOGE(TAG, "GET %s → 404 (bot token is invalid)", path);
    } else if (st == 401) {
        ESP_LOGE(TAG, "GET %s → 401 (bot token unauthorized)", path);
    }
    esp_http_client_cleanup(c);
    return err;
}
