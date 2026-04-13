#include "tg_bot.h"
#include "tg_http.h"
#include "tg_send.h"
#include "tg_parse.h"
#include "tg_ui.h"
#include "config_store.h"
#include "ota.h"
#include "wifi_manager.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_system.h"
#include "lwip/dns.h"
#include "lwip/ip4_addr.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* FreeRTOS 11 compat: pdMS_TO_TICKS may not be defined */
#ifndef pdMS_TO_TICKS
#define pdMS_TO_TICKS(ms) pdMS_TO_TICKS(ms)
#endif

static const char *TAG = "tg_bot";

/* ------------------------------------------------------------------ */
/*  Constants                                                          */
/* ------------------------------------------------------------------ */

#define RESP_BUF_SZ             2048
#define POLL_OK_S               5
#define POLL_FAIL_S             30
#define OFFSET_BUMP_ON_REBOOT   100000
#define NVS_NS                 "tg_bot"

/* ------------------------------------------------------------------ */
/*  State                                                              */
/* ------------------------------------------------------------------ */

static char *s_resp_buf = NULL;
static bool s_dns_ok = false;
static int s_update_offset = -1;
static bool s_commands_registered = false;

/* ------------------------------------------------------------------ */
/*  NVS persistence                                                    */
/* ------------------------------------------------------------------ */

static void load_state(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        int32_t v;
        if (nvs_get_i32(h, "upd_offset", &v) == ESP_OK)
            s_update_offset = (int)v;
        nvs_close(h);
    }
}

static void save_state(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, "upd_offset", (int32_t)s_update_offset);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* ------------------------------------------------------------------ */
/*  Command / callback handlers                                        */
/* ------------------------------------------------------------------ */

static void clear_pending(void)
{
    ESP_LOGI(TAG, "Clearing pending updates");
    tg_http_post_json("/deleteWebhook", "{\"drop_pending_updates\":true}");
    vTaskDelay(pdMS_TO_TICKS(500));
    s_update_offset += OFFSET_BUMP_ON_REBOOT;
    save_state();
}

static void do_reboot(void)
{
    clear_pending();
    ESP_LOGI(TAG, "Rebooting");
    esp_restart();
}

static void handle_cmd(const char *chat, const char *cmd)
{
    if (!s_resp_buf) return;

    if (strcmp(cmd, "start") == 0 || strcmp(cmd, "help") == 0) {
        snprintf(s_resp_buf, RESP_BUF_SZ,
            "<b>🏭 ESP-IDMS Bot</b>\n\n"
            "Industrial Device Monitoring System\n"
            "Firmware: %s\n\nUse the menu below.", ota_get_version());
        tg_send_kb(chat, s_resp_buf, TG_KB_MAIN);
    } else if (strcmp(cmd, "status") == 0) {
        tg_build_status(s_resp_buf, RESP_BUF_SZ);
        tg_send_kb(chat, s_resp_buf, TG_KB_MAIN);
    } else if (strcmp(cmd, "test") == 0) {
        tg_broadcast_alert(
            "🔔 <b>TEST ALERT</b>\n\n"
            "Test notification from ESP-IDMS.\n"
            "If you hear a sound, alerts work! ✅");
        snprintf(s_resp_buf, RESP_BUF_SZ, "✅ Test alert sent.");
        tg_send_kb(chat, s_resp_buf, TG_KB_MAIN);
    } else if (strcmp(cmd, "ota") == 0) {
        tg_build_ota(s_resp_buf, RESP_BUF_SZ);
        tg_send_kb(chat, s_resp_buf, TG_KB_OTA);
    } else if (strcmp(cmd, "reboot") == 0) {
        tg_broadcast_alert("⚠️ <b>REBOOT</b>\n\nRebooting now...");
        vTaskDelay(pdMS_TO_TICKS(1000));
        do_reboot();
    } else if (strcmp(cmd, "techs") == 0) {
        tg_build_techs(s_resp_buf, RESP_BUF_SZ);
        tg_send_kb(chat, s_resp_buf, TG_KB_TECHS);
    } else {
        tg_send_kb(chat, "❓ Unknown. Use /start.", TG_KB_MAIN);
    }
}

static void handle_cb(const char *chat, const char *cb_id, const char *data)
{
    if (!s_resp_buf) return;

    /* Answer callback — log failure but don't retry (400 = expired/duplicate) */
    esp_err_t e = tg_answer_cb(cb_id);
    if (e != ESP_OK) {
        ESP_LOGD(TAG, "answerCallbackQuery failed for %s (expired or dup)", data);
    }

    if (strcmp(data, "cmd_status") == 0) {
        tg_build_status(s_resp_buf, RESP_BUF_SZ);
        tg_send_kb(chat, s_resp_buf, TG_KB_MAIN);
    } else if (strcmp(data, "cmd_test") == 0) {
        tg_broadcast_alert("🔔 <b>TEST ALERT</b>\n\nTest notification. ✅");
        snprintf(s_resp_buf, RESP_BUF_SZ, "✅ Test alert sent.");
        tg_send_kb(chat, s_resp_buf, TG_KB_MAIN);
    } else if (strcmp(data, "cmd_ota") == 0) {
        tg_build_ota(s_resp_buf, RESP_BUF_SZ);
        tg_send_kb(chat, s_resp_buf, TG_KB_OTA);
    } else if (strcmp(data, "cmd_reboot") == 0) {
        tg_broadcast_alert("⚠️ <b>REBOOT</b>\n\nRebooting now...");
        vTaskDelay(pdMS_TO_TICKS(1000));
        do_reboot();
    } else if (strcmp(data, "cmd_techs") == 0) {
        tg_build_techs(s_resp_buf, RESP_BUF_SZ);
        tg_send_kb(chat, s_resp_buf, TG_KB_TECHS);
    } else if (strcmp(data, "ota_status") == 0) {
        snprintf(s_resp_buf, RESP_BUF_SZ,
            "<b>📡 OTA</b>\nVersion: %s\nPartition: %s\nState: %s",
            ota_get_version(), ota_get_partition(), ota_get_status());
        tg_send_kb(chat, s_resp_buf, TG_KB_OTA);
    } else if (strcmp(data, "ota_url") == 0) {
        char ip[16];
        wifi_manager_get_ip(ip, sizeof(ip));
        snprintf(s_resp_buf, RESP_BUF_SZ,
            "<b>🌐 OTA URL</b>\n\n<code>http://%s:%d/</code>\n"
            "User: <code>%s</code>\nPass: <code>%s</code>",
            ip[0] ? ip : "?.?.?.?", CONFIG_IDMS_OTA_HTTP_PORT,
            CONFIG_IDMS_OTA_HTTP_AUTH_USER, CONFIG_IDMS_OTA_HTTP_AUTH_PASS);
        tg_send_kb(chat, s_resp_buf, TG_KB_OTA);
    } else if (strcmp(data, "tech_list") == 0) {
        tg_build_techs(s_resp_buf, RESP_BUF_SZ);
        tg_send_kb(chat, s_resp_buf, TG_KB_TECHS);
    } else if (strcmp(data, "back_main") == 0) {
        snprintf(s_resp_buf, RESP_BUF_SZ, "🏭 <b>ESP-IDMS</b>\nChoose:");
        tg_send_kb(chat, s_resp_buf, TG_KB_MAIN);
    } else {
        tg_send_kb(chat, "❓ Unknown.", TG_KB_MAIN);
    }
}

/* ------------------------------------------------------------------ */
/*  DNS check                                                          */
/* ------------------------------------------------------------------ */

bool tg_check_dns(void)
{
    if (s_dns_ok) return true;
    for (int i = 0; i < 30; i++) {
        ip_addr_t r;
        err_t err = dns_gethostbyname("api.telegram.org", &r, NULL, NULL);
        if (err == ERR_OK) {
            s_dns_ok = true;
            char ip_s[16];
            ip4addr_ntoa_r(ip_2_ip4(&r), ip_s, sizeof(ip_s));
            ESP_LOGI(TAG, "DNS OK: %s", ip_s);
            return true;
        } else if (err == ERR_INPROGRESS) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            err = dns_gethostbyname("api.telegram.org", &r, NULL, NULL);
            if (err == ERR_OK) {
                s_dns_ok = true;
                char ip_s[16];
                ip4addr_ntoa_r(ip_2_ip4(&r), ip_s, sizeof(ip_s));
                ESP_LOGI(TAG, "DNS OK (delayed): %s", ip_s);
                return true;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGE(TAG, "DNS FAIL: api.telegram.org");
    return false;
}

/* ------------------------------------------------------------------ */
/*  Bot command registration (retried on recovery)                     */
/* ------------------------------------------------------------------ */

static void register_cmds(void)
{
    if (s_commands_registered) return;
    esp_err_t e = tg_http_post_json("/setMyCommands",
        "{\"commands\":["
        "{\"command\":\"start\",\"description\":\"Main menu\"},"
        "{\"command\":\"status\",\"description\":\"Device status report\"},"
        "{\"command\":\"ota\",\"description\":\"OTA firmware update\"},"
        "{\"command\":\"test\",\"description\":\"Send test alert\"},"
        "{\"command\":\"reboot\",\"description\":\"Reboot device\"},"
        "{\"command\":\"techs\",\"description\":\"Manage technician IDs\"}"
        "]}");
    if (e == ESP_OK) {
        s_commands_registered = true;
        ESP_LOGI(TAG, "Bot commands registered");
    }
}

/* ------------------------------------------------------------------ */
/*  Poll task                                                          */
/* ------------------------------------------------------------------ */

static void poll_task(void *arg)
{
    (void)arg;
    if (CONFIG_IDMS_TELEGRAM_BOT_TOKEN[0] == '\0') {
        ESP_LOGW(TAG, "Bot token not set");
        vTaskDelete(NULL);
        return;
    }

    load_state();

    char *resp = malloc(4096);
    if (!resp) { ESP_LOGE(TAG, "OOM resp"); vTaskDelete(NULL); return; }
    s_resp_buf = malloc(RESP_BUF_SZ);
    if (!s_resp_buf) { ESP_LOGE(TAG, "OOM s_resp_buf"); free(resp); vTaskDelete(NULL); return; }

    ESP_LOGI(TAG, "Bot started (offset=%d)", s_update_offset);

    if (!tg_check_dns()) {
        ESP_LOGW(TAG, "DNS unavailable — bot disabled");
        free(resp); free(s_resp_buf); s_resp_buf = NULL;
        vTaskDelete(NULL);
        return;
    }

    int poll_int = POLL_OK_S;
    int fail_n = 0;
    bool was_off = false;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(poll_int * 1000));

        char path[256];
        if (s_update_offset < 0) {
            snprintf(path, sizeof(path),
                     "/getUpdates?allowed_updates=[\"message\",\"callback_query\"]&limit=1");
        } else {
            snprintf(path, sizeof(path),
                     "/getUpdates?allowed_updates=[\"message\",\"callback_query\"]&offset=%d&limit=10",
                     s_update_offset);
        }

        int st = 0;
        resp[0] = '\0';
        esp_err_t err = tg_http_get(path, resp, 4096, &st);

        if (err != ESP_OK || resp[0] == '\0') {
            fail_n++;
            poll_int = POLL_FAIL_S;
            if (fail_n == 1) {
                ESP_LOGW(TAG, "Connection lost — polling every %d s", POLL_FAIL_S);
                was_off = true;
            }
            continue;
        }

        /* Recovered */
        if (was_off && fail_n > 0) {
            ESP_LOGI(TAG, "Connection restored after %d failures", fail_n);
            tg_flush_offline_queue();
            register_cmds();
            was_off = false;
        }
        fail_n = 0;
        poll_int = POLL_OK_S;

        /* Advance offset + persist */
        char uid[16];
        if (tg_json_val(resp, "\"update_id\":", uid, sizeof(uid))) {
            s_update_offset = atoi(uid) + 1;
            save_state();
        }

        /* Extract chat */
        const char *chat = tg_json_chat(resp);
        if (!chat) {
            ESP_LOGD(TAG, "No chat_id in response");
            continue;
        }

        /* ---- Callback query ---- */
        if (tg_is_cb(resp)) {
            /* Auto-register chat ID for callbacks too */
            config_add_tech_id(chat);

            char cb_id[64];
            const char *cbid = tg_json_cb_id(resp, cb_id, sizeof(cb_id));
            char cb_data[64];
            const char *data = tg_json_val(resp, "\"data\":", cb_data, sizeof(cb_data));
            if (chat && cbid && data) {
                if (!tg_is_authorized(resp)) {
                    ESP_LOGW(TAG, "Unauthorized callback from %s (id=%s)", chat, cbid);
                    tg_answer_cb(cbid);
                    continue;
                }
                ESP_LOGI(TAG, "Callback: %s", data);
                handle_cb(chat, cbid, data);
            }
            continue;
        }

        /* ---- Text message ---- */
        if (tg_is_msg(resp)) {
            config_add_tech_id(chat);
            if (!tg_is_authorized(resp)) {
                ESP_LOGW(TAG, "Unauthorized sender %s — ignored", chat);
                continue;
            }

            if (tg_is_cmd(resp, "start") || tg_is_cmd(resp, "help"))
                handle_cmd(chat, "start");
            else if (tg_is_cmd(resp, "status"))
                handle_cmd(chat, "status");
            else if (tg_is_cmd(resp, "test"))
                handle_cmd(chat, "test");
            else if (tg_is_cmd(resp, "ota"))
                handle_cmd(chat, "ota");
            else if (tg_is_cmd(resp, "reboot"))
                handle_cmd(chat, "reboot");
            else if (tg_is_cmd(resp, "techs"))
                handle_cmd(chat, "techs");
            else
                tg_send_kb(chat, "👋 Use /start for menu.", TG_KB_MAIN);
        }
    }
}

void tg_bot_start(void)
{
    xTaskCreatePinnedToCore(poll_task, "tg_bot", 12288, NULL, 3, NULL, tskNO_AFFINITY);
}
