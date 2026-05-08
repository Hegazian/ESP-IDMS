#include "tg_bot.h"
#include "tg_http.h"
#include "tg_send.h"
#include "tg_parse.h"
#include "tg_ui.h"
#include "tg_token.h"
#include "config_store.h"
#include "ota.h"
#include "wifi_manager.h"
#include "telegram.h"
#include "telemetry.h"
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

static const char *TAG = "tg_bot";

#define RESP_BUF_SZ             2048
#define POLL_OK_S               5
#define POLL_FAIL_S             30
#define OFFSET_BUMP_ON_REBOOT   100000
#define NVS_NS                 "tg_bot"
#define DNS_RETRY_INTERVAL_S   60

static char *s_resp_buf = NULL;
static bool s_dns_ok = false;
static int s_update_offset = -1;
static bool s_commands_registered = false;
static TaskHandle_t s_poll_task = NULL;

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
            "<b>\xf0\x9f\x8f\xad ESP-IDMS Bot</b>\n\n"
            "Industrial Device Monitoring System\n"
            "Firmware: %s\n\nUse the menu below.", ota_get_version());
        tg_send_kb(chat, s_resp_buf, TG_KB_MAIN);
    } else if (strcmp(cmd, "status") == 0) {
        tg_build_status(s_resp_buf, RESP_BUF_SZ);
        tg_send_kb(chat, s_resp_buf, TG_KB_MAIN);
        telegram_cancel_alerts(chat);
    } else if (strcmp(cmd, "weekly") == 0) {
        tg_build_weekly(s_resp_buf, RESP_BUF_SZ);
        tg_send_kb(chat, s_resp_buf, TG_KB_MAIN);
    } else if (strcmp(cmd, "export") == 0) {
        tg_build_export(s_resp_buf, RESP_BUF_SZ);
        tg_send_kb(chat, s_resp_buf, TG_KB_MAIN);
    } else if (strcmp(cmd, "test") == 0) {
        tg_broadcast_alert(
            "\xf0\x9f\x94\x94 <b>TEST ALERT</b>\n\n"
            "Test notification from ESP-IDMS.\n"
            "If you hear a sound, alerts work! \xe2\x9c\x85");
        snprintf(s_resp_buf, RESP_BUF_SZ, "\xe2\x9c\x85 Test alert sent.");
        tg_send_kb(chat, s_resp_buf, TG_KB_MAIN);
    } else if (strcmp(cmd, "ota") == 0) {
        tg_build_ota(s_resp_buf, RESP_BUF_SZ);
        tg_send_kb(chat, s_resp_buf, TG_KB_OTA);
    } else if (strcmp(cmd, "reboot") == 0) {
        tg_build_reboot_confirm(s_resp_buf, RESP_BUF_SZ);
        tg_send_kb(chat, s_resp_buf, TG_KB_REBOOT_CONFIRM);
    } else if (strcmp(cmd, "techs") == 0) {
        tg_build_techs(s_resp_buf, RESP_BUF_SZ);
        tg_send_kb(chat, s_resp_buf, TG_KB_TECHS);
    } else if (strncmp(cmd, "remove_tech", 11) == 0) {
        const char *idx_str = cmd + 11;
        while (*idx_str == ' ') idx_str++;
        int idx = atoi(idx_str);
        uint8_t count = config_get_tech_count();
        if (idx < 0 || idx >= count) {
            snprintf(s_resp_buf, RESP_BUF_SZ, "\xe2\x9d\x8c Invalid index. Use /techs to list.");
            tg_send_kb(chat, s_resp_buf, TG_KB_TECHS);
        } else {
            nvs_handle_t h;
            if (nvs_open("idms", NVS_READWRITE, &h) == ESP_OK) {
                char removed_id[64] = "";
                size_t rlen = sizeof(removed_id);
                char rkey[16];
                snprintf(rkey, sizeof(rkey), "tech_id_%d", idx);
                nvs_get_str(h, rkey, removed_id, &rlen);

                for (int i = idx; i < count - 1; i++) {
                    char src_key[16], dst_key[16];
                    snprintf(src_key, sizeof(src_key), "tech_id_%d", i + 1);
                    snprintf(dst_key, sizeof(dst_key), "tech_id_%d", i);
                    char val[64];
                    size_t len = sizeof(val);
                    if (nvs_get_str(h, src_key, val, &len) == ESP_OK) {
                        nvs_set_str(h, dst_key, val);
                    }
                }
                char last_key[16];
                snprintf(last_key, sizeof(last_key), "tech_id_%d", count - 1);
                nvs_erase_key(h, last_key);
                nvs_set_u8(h, "tech_count", count - 1);
                nvs_commit(h);
                nvs_close(h);
                snprintf(s_resp_buf, RESP_BUF_SZ,
                    "\xe2\x9c\x85 Removed technician [%d]: <code>%s</code>\nRemaining: %u/5",
                    idx, removed_id, count - 1);
            } else {
                snprintf(s_resp_buf, RESP_BUF_SZ, "\xe2\x9d\x8c Failed to access NVS.");
            }
            tg_send_kb(chat, s_resp_buf, TG_KB_TECHS);
        }
    } else {
        tg_send_kb(chat, "\xe2\x9d\x93 Unknown. Use /start.", TG_KB_MAIN);
    }
}

static void handle_cb(const char *chat, const char *cb_id, const char *data)
{
    if (!s_resp_buf) return;

    esp_err_t e = tg_answer_cb(cb_id);
    if (e != ESP_OK) {
        ESP_LOGD(TAG, "answerCallbackQuery failed for %s (expired or dup)", data);
    }

    if (strcmp(data, "cmd_status") == 0) {
        tg_build_status(s_resp_buf, RESP_BUF_SZ);
        tg_send_kb(chat, s_resp_buf, TG_KB_MAIN);
        telegram_cancel_alerts(chat);
    } else if (strcmp(data, "cmd_weekly") == 0) {
        tg_build_weekly(s_resp_buf, RESP_BUF_SZ);
        tg_send_kb(chat, s_resp_buf, TG_KB_MAIN);
    } else if (strcmp(data, "cmd_export") == 0) {
        tg_build_export(s_resp_buf, RESP_BUF_SZ);
        tg_send_kb(chat, s_resp_buf, TG_KB_MAIN);
    } else if (strcmp(data, "cmd_test") == 0) {
        tg_broadcast_alert("\xf0\x9f\x94\x94 <b>TEST ALERT</b>\n\nTest notification. \xe2\x9c\x85");
        snprintf(s_resp_buf, RESP_BUF_SZ, "\xe2\x9c\x85 Test alert sent.");
        tg_send_kb(chat, s_resp_buf, TG_KB_MAIN);
    } else if (strcmp(data, "cmd_ota") == 0) {
        tg_build_ota(s_resp_buf, RESP_BUF_SZ);
        tg_send_kb(chat, s_resp_buf, TG_KB_OTA);
    } else if (strcmp(data, "cmd_reboot") == 0) {
        tg_build_reboot_confirm(s_resp_buf, RESP_BUF_SZ);
        tg_send_kb(chat, s_resp_buf, TG_KB_REBOOT_CONFIRM);
    } else if (strcmp(data, "cmd_techs") == 0) {
        tg_build_techs(s_resp_buf, RESP_BUF_SZ);
        tg_send_kb(chat, s_resp_buf, TG_KB_TECHS);
    } else if (strcmp(data, "confirm_reboot") == 0) {
        tg_broadcast_alert("\xe2\x9a\xa0\xef\xb8\x8f <b>REBOOT</b>\n\nRebooting now...");
        snprintf(s_resp_buf, RESP_BUF_SZ, "\xe2\x9c\x85 Rebooting device...");
        tg_send_kb(chat, s_resp_buf, TG_KB_MAIN);
        vTaskDelay(pdMS_TO_TICKS(1000));
        do_reboot();
    } else if (strcmp(data, "cancel_reboot") == 0) {
        snprintf(s_resp_buf, RESP_BUF_SZ, "\xf0\x9f\x8f\xad <b>ESP-IDMS</b>\nReboot cancelled. Choose:");
        tg_send_kb(chat, s_resp_buf, TG_KB_MAIN);
    } else if (strcmp(data, "ota_status") == 0) {
        snprintf(s_resp_buf, RESP_BUF_SZ,
            "<b>\xf0\x9f\x93\xa1 OTA</b>\nVersion: %s\nPartition: %s\nState: %s",
            ota_get_version(), ota_get_partition(), ota_get_status());
        tg_send_kb(chat, s_resp_buf, TG_KB_OTA);
    } else if (strcmp(data, "ota_url") == 0) {
        tg_build_ota(s_resp_buf, RESP_BUF_SZ);
        tg_send_kb(chat, s_resp_buf, TG_KB_OTA);
    } else if (strcmp(data, "tech_list") == 0) {
        tg_build_techs(s_resp_buf, RESP_BUF_SZ);
        tg_send_kb(chat, s_resp_buf, TG_KB_TECHS);
    } else if (strcmp(data, "tech_remove") == 0) {
        tg_build_tech_remove(s_resp_buf, RESP_BUF_SZ);
        {
            char kb[512] = "[";
            uint8_t count = config_get_tech_count();
            for (int i = 0; i < count; i++) {
                char btn[96];
                snprintf(btn, sizeof(btn), "[{\"text\":\"Remove [%d]\",\"callback_data\":\"rm_%d\"}]%s",
                         i, i, (i < count - 1) ? "," : "");
                size_t cur = strlen(kb);
                size_t blen = strlen(btn);
                if (cur + blen < sizeof(kb) - 64) {
                    memcpy(kb + cur, btn, blen + 1);
                }
            }
            size_t klen = strlen(kb);
            snprintf(kb + klen, sizeof(kb) - klen, "%s[{\"text\":\"Back\",\"callback_data\":\"back_main\"}]]",
                     count > 0 ? "," : "");
            tg_send_kb(chat, s_resp_buf, kb);
        }
    } else if (strncmp(data, "rm_", 3) == 0) {
        int idx = atoi(data + 3);
        nvs_handle_t h;
        if (nvs_open("idms", NVS_READWRITE, &h) == ESP_OK) {
            uint8_t count = config_get_tech_count();
            if (idx >= 0 && idx < count) {
                char removed_id[64] = "";
                size_t rlen = sizeof(removed_id);
                char rkey[16];
                snprintf(rkey, sizeof(rkey), "tech_id_%d", idx);
                nvs_get_str(h, rkey, removed_id, &rlen);

                for (int i = idx; i < count - 1; i++) {
                    char src_key[16], dst_key[16];
                    snprintf(src_key, sizeof(src_key), "tech_id_%d", i + 1);
                    snprintf(dst_key, sizeof(dst_key), "tech_id_%d", i);
                    char val[64];
                    size_t len = sizeof(val);
                    if (nvs_get_str(h, src_key, val, &len) == ESP_OK) {
                        nvs_set_str(h, dst_key, val);
                    }
                }
                char last_key[16];
                snprintf(last_key, sizeof(last_key), "tech_id_%d", count - 1);
                nvs_erase_key(h, last_key);
                nvs_set_u8(h, "tech_count", count - 1);
                nvs_commit(h);
                snprintf(s_resp_buf, RESP_BUF_SZ,
                    "\xe2\x9c\x85 Removed technician [%d]: <code>%s</code>\nRemaining: %u/5",
                    idx, removed_id, count - 1);
            } else {
                snprintf(s_resp_buf, RESP_BUF_SZ, "\xe2\x9d\x8c Invalid index.");
            }
            nvs_close(h);
        } else {
            snprintf(s_resp_buf, RESP_BUF_SZ, "\xe2\x9d\x8c NVS access failed.");
        }
        tg_send_kb(chat, s_resp_buf, TG_KB_TECHS);
    } else if (strcmp(data, "back_main") == 0) {
        snprintf(s_resp_buf, RESP_BUF_SZ, "\xf0\x9f\x8f\xad <b>ESP-IDMS</b>\nChoose:");
        tg_send_kb(chat, s_resp_buf, TG_KB_MAIN);
    } else {
        tg_send_kb(chat, "\xe2\x9d\x93 Unknown.", TG_KB_MAIN);
    }
}

bool tg_check_dns(void)
{
    if (s_dns_ok) return true;
    for (int i = 0; i < 10; i++) {
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
    ESP_LOGW(TAG, "DNS not yet available");
    return false;
}

static void register_cmds(void)
{
    if (s_commands_registered) return;
    esp_err_t e = tg_http_post_json("/setMyCommands",
        "{\"commands\":["
        "{\"command\":\"start\",\"description\":\"Main menu\"},"
        "{\"command\":\"status\",\"description\":\"Device status report\"},"
        "{\"command\":\"weekly\",\"description\":\"Weekly telemetry report\"},"
        "{\"command\":\"export\",\"description\":\"Export telemetry CSV\"},"
        "{\"command\":\"ota\",\"description\":\"OTA firmware update\"},"
        "{\"command\":\"test\",\"description\":\"Send test alert\"},"
        "{\"command\":\"reboot\",\"description\":\"Reboot device\"},"
        "{\"command\":\"techs\",\"description\":\"Manage technician IDs\"},"
        "{\"command\":\"remove_tech\",\"description\":\"Remove technician by index (e.g. /remove_tech 0)\"}"
        "]}");
    if (e == ESP_OK) {
        s_commands_registered = true;
        ESP_LOGI(TAG, "Bot commands registered");
    }
}

static void poll_task(void *arg)
{
    (void)arg;

    {
        char token[128];
        tg_get_token(token, sizeof(token));
        if (token[0] == '\0') {
            ESP_LOGW(TAG, "Bot token not set — bot disabled");
            vTaskDelete(NULL);
            return;
        }
        size_t tlen = strlen(token);
        ESP_LOGI(TAG, "Bot token: %zu chars, starts '%.5s...%s'",
                 tlen, token, tlen > 8 ? &token[tlen - 3] : "");
        if (tlen < 20) {
            ESP_LOGE(TAG, "Token too short (%zu chars) — expected ~45 chars. Set via serial: set_token <your_token>", tlen);
            vTaskDelete(NULL);
            return;
        }
    }

    load_state();

    char *resp = malloc(8192);
    if (!resp) { ESP_LOGE(TAG, "OOM resp"); vTaskDelete(NULL); return; }
    s_resp_buf = malloc(RESP_BUF_SZ);
    if (!s_resp_buf) { ESP_LOGE(TAG, "OOM s_resp_buf"); free(resp); vTaskDelete(NULL); return; }

    ESP_LOGI(TAG, "Bot started (offset=%d)", s_update_offset);

    int poll_int = POLL_OK_S;
    int fail_n = 0;
    bool was_off = false;
    bool dns_available = false;
    bool boot_notified = false;

    for (;;) {
        if (!dns_available) {
            dns_available = tg_check_dns();
            if (!dns_available) {
                ESP_LOGI(TAG, "DNS unavailable, retrying in %d s", DNS_RETRY_INTERVAL_S);
                vTaskDelay(pdMS_TO_TICKS(DNS_RETRY_INTERVAL_S * 1000));
                continue;
            }
            register_cmds();
            if (!boot_notified) {
                boot_notified = true;
                char _bip[16] = {0};
                wifi_manager_get_ip(_bip, sizeof(_bip));
                char boot_msg[512];
                snprintf(boot_msg, sizeof(boot_msg),
                    "\xf0\x9f\x9f\xa2 <b>ESP-IDMS Online</b>\n\n"
                    "Device started successfully.\n"
                    "Firmware: %s\n"
                    "IP: %s\n\n"
                    "Monitoring is active. Use the menu below.",
                    ota_get_version(), _bip[0] ? _bip : "N/A");
                uint8_t n = config_get_tech_count();
                for (int i = 0; i < n; i++) {
                    char id[64];
                    if (config_get_tech_id(i, id, sizeof(id)) == ESP_OK) {
                        tg_send_kb(id, boot_msg, TG_KB_MAIN);
                    }
                }
                ESP_LOGI(TAG, "Boot notification sent to %u technicians", n);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(poll_int * 1000));

        tg_send_process_reminders();
        if (telemetry_weekly_report_due()) {
            tg_build_weekly(s_resp_buf, RESP_BUF_SZ);
            esp_err_t report_err = tg_broadcast_text(s_resp_buf);
            if (report_err != ESP_OK) {
                ESP_LOGW(TAG, "Weekly report broadcast returned %s", esp_err_to_name(report_err));
            } else {
                telemetry_mark_weekly_report_sent();
            }
        }

        char path[256];
        if (s_update_offset < 0) {
            snprintf(path, sizeof(path),
                     "/getUpdates?allowed_updates=[\"message\",\"callback_query\"]&limit=1");
        } else {
            snprintf(path, sizeof(path),
                     "/getUpdates?allowed_updates=[\"message\",\"callback_query\"]&offset=%d&limit=1",
                     s_update_offset);
        }

        int st = 0;
        resp[0] = '\0';
        esp_err_t err = tg_http_get(path, resp, 8192, &st);

        if (err != ESP_OK || resp[0] == '\0') {
            fail_n++;
            poll_int = POLL_FAIL_S;
            if (fail_n == 1) {
                ESP_LOGW(TAG, "Connection lost — polling every %d s", POLL_FAIL_S);
                was_off = true;
            }
            if (fail_n > 10) {
                s_dns_ok = false;
                dns_available = false;
            }
            continue;
        }

        if (st != 200) {
            ESP_LOGW(TAG, "getUpdates HTTP %d", st);
            continue;
        }

        if (was_off && fail_n > 0) {
            ESP_LOGI(TAG, "Connection restored after %d failures", fail_n);
            tg_flush_offline_queue();
            register_cmds();
            was_off = false;
        }
        fail_n = 0;
        poll_int = POLL_OK_S;

        tg_update_t update;
        if (!tg_parse_update(resp, &update)) {
            int bad_update_id = 0;
            if (tg_parse_first_update_id(resp, &bad_update_id)) {
                ESP_LOGW(TAG, "Dropping unparseable update %d to keep polling moving", bad_update_id);
                s_update_offset = bad_update_id + 1;
                save_state();
            }
            continue;
        }

        ESP_LOGI(TAG, "Update %d: msg=%d cb=%d from=%s chat=%s text='%.40s'",
                 update.update_id, update.is_message, update.is_callback,
                 update.from_id, update.chat_id, update.message_text);

        if (update.update_id > 0) {
            s_update_offset = update.update_id + 1;
            save_state();
        }

        if (update.chat_id[0] == '\0') {
            ESP_LOGW(TAG, "No chat_id in update %d", update.update_id);
            continue;
        }

        if (!tg_is_authorized_id(update.from_id)) {
            uint8_t tech_count = config_get_tech_count();
#if CONFIG_IDMS_TELEGRAM_ALLOW_FIRST_USER_CLAIM
            if (tech_count == 0 && update.is_message &&
                (tg_is_cmd_text(update.message_text, "start") || tg_is_cmd_text(update.message_text, "help"))) {
                esp_err_t add_err = config_add_tech_id(update.from_id);
                if (add_err == ESP_OK) {
                    ESP_LOGI(TAG, "First user auto-registered: from_id=%s", update.from_id);
                    snprintf(s_resp_buf, RESP_BUF_SZ,
                        "\xe2\x9c\x85 <b>Welcome!</b>\n\n"
                        "You are the first registered technician (ID: <code>%s</code>).\n"
                        "You will receive all alerts. Use the menu below.",
                        update.from_id);
                    tg_send_kb(update.chat_id, s_resp_buf, TG_KB_MAIN);
                    continue;
                } else {
                    ESP_LOGE(TAG, "Failed to auto-register first user: %s", esp_err_to_name(add_err));
                }
            }

#endif

            ESP_LOGW(TAG, "Unauthorized %s (from_id=%s, techs=%u) — ignored",
                     update.is_callback ? "callback" : "message",
                     update.from_id, tech_count);
            if (update.is_callback && update.callback_id[0]) {
                tg_answer_cb(update.callback_id);
            }
            snprintf(s_resp_buf, RESP_BUF_SZ,
                "\xe2\x9d\x8c <b>Access Denied</b>\n\n"
                "Your Telegram ID: <code>%s</code>\n"
                "You are not registered as a technician.\n"
                "Ask an admin to add your ID via serial console:\n"
                "<code>add %s</code>",
                update.from_id, update.from_id);
            tg_send_text(update.chat_id, s_resp_buf);
            continue;
        }

        if (update.is_callback) {
            ESP_LOGI(TAG, "Callback: %s", update.callback_data);
            handle_cb(update.chat_id, update.callback_id, update.callback_data);
        } else if (update.is_message) {
            if (tg_is_cmd_text(update.message_text, "start") || tg_is_cmd_text(update.message_text, "help"))
                handle_cmd(update.chat_id, "start");
            else if (tg_is_cmd_text(update.message_text, "status"))
                handle_cmd(update.chat_id, "status");
            else if (tg_is_cmd_text(update.message_text, "weekly"))
                handle_cmd(update.chat_id, "weekly");
            else if (tg_is_cmd_text(update.message_text, "export"))
                handle_cmd(update.chat_id, "export");
            else if (tg_is_cmd_text(update.message_text, "test"))
                handle_cmd(update.chat_id, "test");
            else if (tg_is_cmd_text(update.message_text, "ota"))
                handle_cmd(update.chat_id, "ota");
            else if (tg_is_cmd_text(update.message_text, "reboot"))
                handle_cmd(update.chat_id, "reboot");
            else if (tg_is_cmd_text(update.message_text, "techs"))
                handle_cmd(update.chat_id, "techs");
            else if (tg_is_cmd_text(update.message_text, "remove_tech"))
                handle_cmd(update.chat_id, update.message_text);
            else
                tg_send_kb(update.chat_id, "\xf0\x9f\x91\x8b Use /start for menu.", TG_KB_MAIN);
        }
    }
}

esp_err_t tg_bot_start(void)
{
    if (s_poll_task) {
        return ESP_OK;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(poll_task, "tg_bot", 24576,
                                            NULL, 3, &s_poll_task, tskNO_AFFINITY);
    if (ok != pdPASS) {
        s_poll_task = NULL;
        ESP_LOGE(TAG, "Failed to start Telegram poll task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
