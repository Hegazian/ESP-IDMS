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
#include "cJSON.h"
#include "lwip/dns.h"
#include "lwip/ip4_addr.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <ctype.h>
#include <stdint.h>
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
#define SESSION_MAX             8
#define SESSION_TTL_MS          (10 * 60 * 1000)
#define PASSWORD_MIN_LEN        6
#define LOGIN_THROTTLE_MAX      8
#define LOGIN_FAIL_WINDOW_MS    (10 * 60 * 1000)
#define LOGIN_USER_LOCK_MS      (5 * 60 * 1000)
#define LOGIN_GLOBAL_LOCK_MS    (2 * 60 * 1000)
#define LOGIN_USER_FAIL_LIMIT   3
#define LOGIN_GLOBAL_FAIL_LIMIT 12
#define RECENT_CALLBACK_MAX     8
#define CALLBACK_DEDUPE_MS      (12 * 1000)

static char *s_resp_buf = NULL;
static bool s_dns_ok = false;
static int s_update_offset = -1;
static bool s_commands_registered = false;
static bool s_bot_qr_refreshed = false;
static TaskHandle_t s_poll_task = NULL;

typedef enum {
    SESSION_NONE = 0,
    SESSION_SETUP_ADMIN_NAME,
    SESSION_SETUP_ADMIN_PASSWORD,
    SESSION_SETUP_ADMIN_CONFIRM,
    SESSION_SETUP_CONTACT,
    SESSION_LOGIN_NAME,
    SESSION_LOGIN_PASSWORD,
    SESSION_LOGIN_CONTACT,
} session_state_t;

typedef enum {
    ACTION_NONE = 0,
    ACTION_OTA,
    ACTION_REBOOT_MENU,
    ACTION_REBOOT_NOW,
    ACTION_REMOVE_MENU,
    ACTION_REMOVE_INDEX,
} sensitive_action_t;

typedef struct {
    bool active;
    char from_id[32];
    char chat_id[32];
    session_state_t state;
    sensitive_action_t action;
    int action_index;
    int attempts;
    int64_t expires_ms;
    char name[CONFIG_TECH_NAME_MAX_LEN + 1];
    char password[CONFIG_TECH_PASSWORD_MAX_LEN + 1];
} tg_session_t;

typedef struct {
    bool active;
    char from_id[32];
    int failures;
    int64_t window_start_ms;
    int64_t lock_until_ms;
} login_throttle_t;

typedef struct {
    bool active;
    char from_id[32];
    char chat_id[32];
    char data[64];
    int64_t seen_ms;
} recent_callback_t;

static tg_session_t s_sessions[SESSION_MAX];
static login_throttle_t s_login_throttle[LOGIN_THROTTLE_MAX];
static recent_callback_t s_recent_callbacks[RECENT_CALLBACK_MAX];
static int s_global_login_failures = 0;
static int64_t s_global_login_window_ms = 0;
static int64_t s_global_login_lock_until_ms = 0;

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

static int64_t now_ms(void)
{
    return (int64_t)xTaskGetTickCount() * portTICK_PERIOD_MS;
}

static void safe_copy(char *dst, size_t dst_len, const char *src)
{
    if (!dst || dst_len == 0) {
        return;
    }
    if (!src) {
        src = "";
    }
    strncpy(dst, src, dst_len - 1);
    dst[dst_len - 1] = '\0';
}

static void trim_copy(char *dst, size_t dst_len, const char *src)
{
    if (!dst || dst_len == 0) {
        return;
    }
    dst[0] = '\0';
    if (!src) {
        return;
    }
    while (*src && isspace((unsigned char)*src)) {
        src++;
    }
    size_t len = strlen(src);
    while (len > 0 && isspace((unsigned char)src[len - 1])) {
        len--;
    }
    if (len >= dst_len) {
        len = dst_len - 1;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static bool valid_name(const char *name)
{
    if (!name || name[0] == '\0') {
        return false;
    }
    size_t len = strlen(name);
    if (len > CONFIG_TECH_NAME_MAX_LEN) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20 || c == 0x7f) {
            return false;
        }
    }
    return true;
}

static bool valid_password(const char *password)
{
    if (!password) {
        return false;
    }
    size_t len = strlen(password);
    return len >= PASSWORD_MIN_LEN && len <= CONFIG_TECH_PASSWORD_MAX_LEN;
}

static void html_escape(char *dst, size_t dst_len, const char *src)
{
    if (!dst || dst_len == 0) {
        return;
    }
    dst[0] = '\0';
    if (!src) {
        return;
    }
    size_t used = 0;
    for (size_t i = 0; src[i] && used + 1 < dst_len; i++) {
        const char *rep = NULL;
        switch (src[i]) {
        case '&': rep = "&amp;"; break;
        case '<': rep = "&lt;"; break;
        case '>': rep = "&gt;"; break;
        case '"': rep = "&quot;"; break;
        default: break;
        }
        if (rep) {
            size_t rlen = strlen(rep);
            if (used + rlen >= dst_len) {
                break;
            }
            memcpy(dst + used, rep, rlen);
            used += rlen;
        } else {
            dst[used++] = src[i];
        }
    }
    dst[used] = '\0';
}

static void clear_session(tg_session_t *s)
{
    if (s) {
        memset(s, 0, sizeof(*s));
    }
}

static tg_session_t *find_session(const char *from_id)
{
    int64_t now = now_ms();
    for (int i = 0; i < SESSION_MAX; i++) {
        if (s_sessions[i].active && s_sessions[i].expires_ms <= now) {
            clear_session(&s_sessions[i]);
        }
        if (s_sessions[i].active && strcmp(s_sessions[i].from_id, from_id) == 0) {
            return &s_sessions[i];
        }
    }
    return NULL;
}

static tg_session_t *get_session(const char *from_id, const char *chat_id, bool create)
{
    tg_session_t *s = find_session(from_id);
    if (s || !create) {
        return s;
    }

    int slot = -1;
    int64_t oldest = INT64_MAX;
    for (int i = 0; i < SESSION_MAX; i++) {
        if (!s_sessions[i].active) {
            slot = i;
            break;
        }
        if (s_sessions[i].expires_ms < oldest) {
            oldest = s_sessions[i].expires_ms;
            slot = i;
        }
    }

    s = &s_sessions[slot];
    memset(s, 0, sizeof(*s));
    s->active = true;
    safe_copy(s->from_id, sizeof(s->from_id), from_id);
    safe_copy(s->chat_id, sizeof(s->chat_id), chat_id);
    s->expires_ms = now_ms() + SESSION_TTL_MS;
    return s;
}

static bool session_waits_for_secret(const char *from_id)
{
    tg_session_t *s = find_session(from_id);
    if (!s) {
        return false;
    }
    return s->state == SESSION_LOGIN_PASSWORD ||
           s->state == SESSION_SETUP_ADMIN_PASSWORD ||
           s->state == SESSION_SETUP_ADMIN_CONFIRM;
}

static bool is_private_chat(const tg_update_t *update)
{
    return update && strcmp(update->chat_type, "private") == 0;
}

static bool callback_seen_recently(const tg_update_t *update)
{
    if (!update || !update->is_callback || update->callback_data[0] == '\0') {
        return false;
    }

    int64_t now = now_ms();
    int slot = -1;
    int64_t oldest = INT64_MAX;
    for (int i = 0; i < RECENT_CALLBACK_MAX; i++) {
        recent_callback_t *r = &s_recent_callbacks[i];
        if (r->active && now - r->seen_ms > CALLBACK_DEDUPE_MS) {
            memset(r, 0, sizeof(*r));
        }
        if (r->active &&
            strcmp(r->from_id, update->from_id) == 0 &&
            strcmp(r->chat_id, update->chat_id) == 0 &&
            strcmp(r->data, update->callback_data) == 0) {
            r->seen_ms = now;
            return true;
        }
        if (!r->active && slot < 0) {
            slot = i;
        } else if (r->active && r->seen_ms < oldest) {
            oldest = r->seen_ms;
            slot = i;
        }
    }

    if (slot >= 0) {
        recent_callback_t *r = &s_recent_callbacks[slot];
        memset(r, 0, sizeof(*r));
        r->active = true;
        safe_copy(r->from_id, sizeof(r->from_id), update->from_id);
        safe_copy(r->chat_id, sizeof(r->chat_id), update->chat_id);
        safe_copy(r->data, sizeof(r->data), update->callback_data);
        r->seen_ms = now;
    }
    return false;
}

static login_throttle_t *get_login_throttle(const char *from_id, bool create)
{
    if (!from_id || from_id[0] == '\0') {
        return NULL;
    }

    int empty_slot = -1;
    int oldest_slot = 0;
    int64_t oldest = INT64_MAX;
    for (int i = 0; i < LOGIN_THROTTLE_MAX; i++) {
        if (s_login_throttle[i].active &&
            strcmp(s_login_throttle[i].from_id, from_id) == 0) {
            return &s_login_throttle[i];
        }
        if (!s_login_throttle[i].active && empty_slot < 0) {
            empty_slot = i;
        } else if (s_login_throttle[i].active &&
                   s_login_throttle[i].window_start_ms < oldest) {
            oldest = s_login_throttle[i].window_start_ms;
            oldest_slot = i;
        }
    }

    if (!create) {
        return NULL;
    }

    int slot = empty_slot >= 0 ? empty_slot : oldest_slot;
    login_throttle_t *t = &s_login_throttle[slot];
    memset(t, 0, sizeof(*t));
    t->active = true;
    safe_copy(t->from_id, sizeof(t->from_id), from_id);
    t->window_start_ms = now_ms();
    return t;
}

static bool login_throttle_blocked(const char *from_id, const char *chat_id)
{
    int64_t now = now_ms();
    if (s_global_login_lock_until_ms > now) {
        tg_send_text(chat_id,
            "\xe2\x9d\x8c Too many failed login attempts. Try again in a few minutes.");
        return true;
    }

    login_throttle_t *t = get_login_throttle(from_id, false);
    if (t && t->lock_until_ms > now) {
        tg_send_text(chat_id,
            "\xe2\x9d\x8c Too many wrong login attempts. Try again in a few minutes.");
        return true;
    }
    return false;
}

static void login_throttle_record_failure(const char *from_id)
{
    int64_t now = now_ms();
    login_throttle_t *t = get_login_throttle(from_id, true);
    if (t) {
        if (now - t->window_start_ms > LOGIN_FAIL_WINDOW_MS) {
            t->window_start_ms = now;
            t->failures = 0;
        }
        t->failures++;
        if (t->failures >= LOGIN_USER_FAIL_LIMIT) {
            t->lock_until_ms = now + LOGIN_USER_LOCK_MS;
            t->failures = 0;
            t->window_start_ms = now;
        }
    }

    if (now - s_global_login_window_ms > LOGIN_FAIL_WINDOW_MS) {
        s_global_login_window_ms = now;
        s_global_login_failures = 0;
    }
    s_global_login_failures++;
    if (s_global_login_failures >= LOGIN_GLOBAL_FAIL_LIMIT) {
        s_global_login_lock_until_ms = now + LOGIN_GLOBAL_LOCK_MS;
        s_global_login_failures = 0;
        s_global_login_window_ms = now;
    }
}

static void login_throttle_reset_user(const char *from_id)
{
    login_throttle_t *t = get_login_throttle(from_id, false);
    if (t) {
        memset(t, 0, sizeof(*t));
    }
}

static void send_main_menu(const char *chat)
{
    snprintf(s_resp_buf, RESP_BUF_SZ,
        "<b>\xf0\x9f\x8f\xad ESP-IDMS Bot</b>\n\n"
        "Industrial Device Monitoring System\n"
        "Firmware: %s\n\nUse the menu below.", ota_get_version());
    tg_send_kb(chat, s_resp_buf, TG_KB_MAIN);
}

static void execute_sensitive_action(const char *chat, const char *from_id,
                                     sensitive_action_t action, int action_index);
static void require_sensitive_action(const char *chat, const char *from_id,
                                     sensitive_action_t action, int action_index);

static void send_serial_login_change_notice(const char *chat)
{
    tg_send_text(chat,
        "\xe2\x9d\x8c <b>Telegram Login Is Locked</b>\n\n"
        "The shared admin name and password can only be created once from Telegram on a blank device.\n"
        "To change them now, use the serial console:\n"
        "<code>set_bot_admin NAME</code>\n"
        "<code>set_bot_password PASSWORD</code>");
}

static bool has_pending_phone_slots(void)
{
    uint8_t count = config_get_tech_count();
    for (int i = 0; i < count; i++) {
        char id[64] = "";
        char phone[24] = "";
        config_get_tech_id(i, id, sizeof(id));
        config_get_tech_phone(i, phone, sizeof(phone));
        if (id[0] == '\0' && phone[0] != '\0') {
            return true;
        }
    }
    return false;
}

static bool handle_contact_authorization(const tg_update_t *update)
{
    if (!update->has_contact) {
        return false;
    }

    tg_session_t *session = find_session(update->from_id);
    bool already_authorized = tg_is_authorized_id(update->from_id);
    bool allow_new_binding = session &&
        (session->state == SESSION_SETUP_CONTACT || session->state == SESSION_LOGIN_CONTACT);
    allow_new_binding = allow_new_binding || already_authorized;

    if (update->contact_user_id[0] != '\0' &&
        strcmp(update->contact_user_id, update->from_id) != 0) {
        tg_send_contact_request(update->chat_id,
            "\xe2\x9d\x8c Please share <b>your own</b> Telegram contact using the button below.");
        return true;
    }

    char phone[24] = "";
    if (config_normalize_egypt_phone(update->contact_phone, phone, sizeof(phone)) != ESP_OK) {
        tg_send_contact_request(update->chat_id,
            "\xe2\x9d\x8c This phone number is not an Egyptian mobile number. "
            "Ask the admin to add your local mobile number on the display, then share contact again.");
        return true;
    }

    int idx = -1;
    if (config_find_tech_phone(phone, &idx) != ESP_OK && !allow_new_binding) {
        tg_send_text(update->chat_id,
            "\xe2\x9d\x8c This phone number is not authorized on this device. "
            "Ask an admin to add your local mobile number from the display.");
        return true;
    }

    char existing_id[64] = "";
    config_get_tech_id(idx, existing_id, sizeof(existing_id));
    if (existing_id[0] != '\0' && strcmp(existing_id, update->from_id) != 0) {
        tg_send_text(update->chat_id,
            "\xe2\x9d\x8c This phone number is already linked to another Telegram account.");
        return true;
    }

    const char *name = (session && session->name[0]) ? session->name : "Phone";
    esp_err_t err = config_bind_tech_phone(phone, update->from_id, name);
    if (err != ESP_OK) {
        snprintf(s_resp_buf, RESP_BUF_SZ,
            "\xe2\x9d\x8c Could not authorize this contact: %s", esp_err_to_name(err));
        tg_send_text(update->chat_id, s_resp_buf);
        return true;
    }

    snprintf(s_resp_buf, RESP_BUF_SZ,
        "\xe2\x9c\x85 <b>Access Granted</b>\n\n"
        "Phone <code>%s</code> is now linked to this Telegram account. "
        "You will not be asked again.", phone);
    if (session) {
        login_throttle_reset_user(update->from_id);
        clear_session(session);
    }
    tg_send_kb(update->chat_id, s_resp_buf, TG_KB_MAIN);
    return true;
}

static void request_contact_authorization(const char *chat_id)
{
    tg_send_contact_request(chat_id,
        "\xf0\x9f\x93\xb1 <b>Phone Verification</b>\n\n"
        "Ask the admin to add your local mobile number on the display, then tap "
        "<b>Share Phone Number</b> below. The device adds <code>+20</code> automatically.");
}

static bool begin_admin_setup(const char *chat_id, const char *from_id)
{
    if (config_has_telegram_admin_credentials() ||
        config_get_tech_count() != 0) {
        send_serial_login_change_notice(chat_id);
        return true;
    }

    tg_session_t *s = get_session(from_id, chat_id, true);
    if (!s) {
        tg_send_text(chat_id, "\xe2\x9d\x8c Could not start setup session.");
        return true;
    }

    s->state = SESSION_SETUP_ADMIN_NAME;
    s->attempts = 0;
    s->name[0] = '\0';
    s->password[0] = '\0';

    tg_send_text(chat_id,
        "\xf0\x9f\x94\x90 <b>Telegram Login Setup</b>\n\n"
        "Create the shared admin name used by technicians to register this bot.\n"
        "Send the admin name now.");
    return true;
}

static bool begin_shared_login(const tg_update_t *update)
{
    if (!config_has_telegram_admin_credentials()) {
        if (config_get_tech_count() == 0) {
            return begin_admin_setup(update->chat_id, update->from_id);
        }

        tg_send_text(update->chat_id,
            "\xe2\x9d\x8c <b>Bot Login Not Configured</b>\n\n"
            "Telegram setup is only allowed when the device has no saved technicians. "
            "Use the serial console to set the shared login:\n"
            "<code>set_bot_admin NAME</code>\n"
            "<code>set_bot_password PASSWORD</code>");
        return true;
    }

    if (login_throttle_blocked(update->from_id, update->chat_id)) {
        return true;
    }

    if (config_get_tech_count() >= CONFIG_TECH_MAX_COUNT) {
        tg_send_text(update->chat_id,
            "\xe2\x9d\x8c Technician list is full. Ask an admin to remove an old technician.");
        return true;
    }

    tg_session_t *s = get_session(update->from_id, update->chat_id, true);
    if (!s) {
        tg_send_text(update->chat_id, "\xe2\x9d\x8c Could not start login session.");
        return true;
    }
    s->state = SESSION_LOGIN_NAME;
    s->attempts = 0;
    s->name[0] = '\0';
    tg_send_text(update->chat_id,
        "\xf0\x9f\x94\x90 <b>Technician Login</b>\n\n"
        "Send the shared admin name.");
    return true;
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

static void send_remove_menu(const char *chat)
{
    tg_build_tech_remove(s_resp_buf, RESP_BUF_SZ);
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

static void remove_tech_at(const char *chat, int idx)
{
    uint8_t count = config_get_tech_count();
    if (idx < 0 || idx >= count) {
        snprintf(s_resp_buf, RESP_BUF_SZ, "\xe2\x9d\x8c Invalid index. Use /techs to list.");
        tg_send_kb(chat, s_resp_buf, TG_KB_TECHS);
        return;
    }

    char removed_id[64] = "";
    char removed_phone[24] = "";
    char removed_name[CONFIG_TECH_NAME_MAX_LEN + 1] = "";
    char esc_name[96] = "";
    config_get_tech_id(idx, removed_id, sizeof(removed_id));
    config_get_tech_phone(idx, removed_phone, sizeof(removed_phone));
    config_get_tech_name(idx, removed_name, sizeof(removed_name));
    html_escape(esc_name, sizeof(esc_name), removed_name);

    esp_err_t err = config_remove_tech(idx);
    if (err == ESP_OK) {
        snprintf(s_resp_buf, RESP_BUF_SZ,
            "\xe2\x9c\x85 Removed technician [%d]: %s<code>%s</code>\nRemaining: %u/%d",
            idx, esc_name[0] ? esc_name : "",
            removed_phone[0] ? removed_phone : removed_id,
            count - 1, CONFIG_TECH_MAX_COUNT);
    } else {
        snprintf(s_resp_buf, RESP_BUF_SZ,
            "\xe2\x9d\x8c Failed to remove technician: %s", esp_err_to_name(err));
    }
    tg_send_kb(chat, s_resp_buf, TG_KB_TECHS);
}

static void require_sensitive_action(const char *chat, const char *from_id,
                                     sensitive_action_t action, int action_index)
{
    execute_sensitive_action(chat, from_id, action, action_index);
}

static void execute_sensitive_action(const char *chat, const char *from_id,
                                     sensitive_action_t action, int action_index)
{
    switch (action) {
    case ACTION_OTA:
        tg_build_ota(s_resp_buf, RESP_BUF_SZ);
        tg_send_kb(chat, s_resp_buf, TG_KB_OTA);
        break;
    case ACTION_REBOOT_MENU:
        tg_build_reboot_confirm(s_resp_buf, RESP_BUF_SZ);
        tg_send_kb(chat, s_resp_buf, TG_KB_REBOOT_CONFIRM);
        break;
    case ACTION_REBOOT_NOW:
        tg_broadcast_alert("\xe2\x9a\xa0\xef\xb8\x8f <b>REBOOT</b>\n\nRebooting now...");
        snprintf(s_resp_buf, RESP_BUF_SZ, "\xe2\x9c\x85 Rebooting device...");
        tg_send_kb(chat, s_resp_buf, TG_KB_MAIN);
        vTaskDelay(pdMS_TO_TICKS(1000));
        do_reboot();
        break;
    case ACTION_REMOVE_MENU:
        send_remove_menu(chat);
        break;
    case ACTION_REMOVE_INDEX:
        remove_tech_at(chat, action_index);
        break;
    case ACTION_NONE:
    default:
        send_main_menu(chat);
        break;
    }
}

static bool handle_session_message(const tg_update_t *update)
{
    tg_session_t *s = find_session(update->from_id);
    if (!s || !update->is_message) {
        return false;
    }

    if (tg_is_cmd_text(update->message_text, "cancel")) {
        clear_session(s);
        tg_send_text(update->chat_id, "Cancelled.");
        return true;
    }

    s->expires_ms = now_ms() + SESSION_TTL_MS;

    char value[CONFIG_TECH_PASSWORD_MAX_LEN + 33];
    trim_copy(value, sizeof(value), update->message_text);

    switch (s->state) {
    case SESSION_SETUP_ADMIN_NAME:
        if (!valid_name(value)) {
            tg_send_text(update->chat_id,
                "\xe2\x9d\x8c Admin name must be 1-32 characters and cannot contain control characters.");
            return true;
        }
        safe_copy(s->name, sizeof(s->name), value);
        s->state = SESSION_SETUP_ADMIN_PASSWORD;
        tg_send_text(update->chat_id,
            "\xf0\x9f\x94\x91 Send the shared admin password.\n\n"
            "Telegram will show this message in chat history, so use a private chat.");
        return true;

    case SESSION_SETUP_ADMIN_PASSWORD:
        if (!valid_password(value)) {
            snprintf(s_resp_buf, RESP_BUF_SZ,
                "\xe2\x9d\x8c Password must be %d-%d characters. Send a new password.",
                PASSWORD_MIN_LEN, CONFIG_TECH_PASSWORD_MAX_LEN);
            tg_send_text(update->chat_id, s_resp_buf);
            return true;
        }
        safe_copy(s->password, sizeof(s->password), value);
        s->state = SESSION_SETUP_ADMIN_CONFIRM;
        tg_send_text(update->chat_id, "\xf0\x9f\x94\x81 Send the same password again to confirm.");
        return true;

    case SESSION_SETUP_ADMIN_CONFIRM: {
        if (strcmp(value, s->password) != 0) {
            s->password[0] = '\0';
            s->state = SESSION_SETUP_ADMIN_PASSWORD;
            tg_send_text(update->chat_id,
                "\xe2\x9d\x8c Passwords did not match. Send the shared admin password again.");
            return true;
        }

        if (config_has_telegram_admin_credentials() || config_get_tech_count() != 0) {
            tg_send_text(update->chat_id,
                "\xe2\x9d\x8c Telegram setup is no longer available. "
                "The shared login was already configured.");
            clear_session(s);
            return true;
        }

        esp_err_t err = config_set_telegram_admin_name(s->name);
        if (err == ESP_OK) {
            err = config_set_telegram_admin_password(s->password);
        }
        if (err != ESP_OK) {
            snprintf(s_resp_buf, RESP_BUF_SZ,
                "\xe2\x9d\x8c Could not save Telegram login setup: %s", esp_err_to_name(err));
            tg_send_text(update->chat_id, s_resp_buf);
            clear_session(s);
            return true;
        }

        s->state = SESSION_SETUP_CONTACT;
        s->password[0] = '\0';
        tg_send_contact_request(update->chat_id,
            "\xe2\x9c\x85 <b>Telegram Login Configured</b>\n\n"
            "Now tap <b>Share Phone Number</b> to link this Telegram account to your phone.");
        return true;
    }

    case SESSION_LOGIN_NAME:
        if (!valid_name(value)) {
            tg_send_text(update->chat_id,
                "\xe2\x9d\x8c Admin name must be 1-32 characters and cannot contain control characters.");
            return true;
        }
        safe_copy(s->name, sizeof(s->name), value);
        s->state = SESSION_LOGIN_PASSWORD;
        tg_send_text(update->chat_id, "\xf0\x9f\x94\x91 Send the shared admin password.");
        return true;

    case SESSION_LOGIN_PASSWORD: {
        bool match = false;
        esp_err_t err = config_check_telegram_admin_credentials(s->name, value, &match);
        if (err == ESP_OK && match) {
            if (config_get_tech_count() >= CONFIG_TECH_MAX_COUNT) {
                tg_send_text(update->chat_id,
                    "\xe2\x9d\x8c Technician list is full. Ask an admin to remove an old technician.");
                clear_session(s);
                return true;
            }
            s->state = SESSION_LOGIN_CONTACT;
            tg_send_contact_request(update->chat_id,
                "\xe2\x9c\x85 Admin name/password accepted.\n\n"
                "Tap <b>Share Phone Number</b> to link this Telegram account to your phone.");
            return true;
        }

        login_throttle_record_failure(update->from_id);
        s->attempts++;
        if (s->attempts >= 3) {
            tg_send_text(update->chat_id, "\xe2\x9d\x8c Too many wrong login attempts. Send /start to try again.");
            clear_session(s);
        } else {
            tg_send_text(update->chat_id, "\xe2\x9d\x8c Wrong admin name or password. Send the admin name again.");
            s->state = SESSION_LOGIN_NAME;
            s->name[0] = '\0';
        }
        return true;
    }

    case SESSION_NONE:
    case SESSION_SETUP_CONTACT:
    case SESSION_LOGIN_CONTACT:
        return false;
    default:
        clear_session(s);
        return false;
    }
}

static void handle_cmd(const char *chat, const char *from_id, const char *cmd)
{
    if (!s_resp_buf) return;

    if (strcmp(cmd, "start") == 0 || strcmp(cmd, "help") == 0) {
        send_main_menu(chat);
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
        require_sensitive_action(chat, from_id, ACTION_OTA, -1);
    } else if (strcmp(cmd, "reboot") == 0) {
        require_sensitive_action(chat, from_id, ACTION_REBOOT_MENU, -1);
    } else if (strcmp(cmd, "techs") == 0) {
        tg_build_techs(s_resp_buf, RESP_BUF_SZ);
        tg_send_kb(chat, s_resp_buf, TG_KB_TECHS);
    } else if (strncmp(cmd, "remove_tech", 11) == 0) {
        const char *idx_str = cmd + 11;
        if (*idx_str == '@') {
            while (*idx_str && !isspace((unsigned char)*idx_str)) {
                idx_str++;
            }
        }
        while (*idx_str == ' ') idx_str++;
        if (!isdigit((unsigned char)*idx_str)) {
            tg_send_kb(chat, "\xe2\x9d\x8c Usage: /remove_tech <index>", TG_KB_TECHS);
            return;
        }
        int idx = atoi(idx_str);
        require_sensitive_action(chat, from_id, ACTION_REMOVE_INDEX, idx);
    } else {
        tg_send_kb(chat, "\xe2\x9d\x93 Unknown. Use /start.", TG_KB_MAIN);
    }
}

static void handle_cb(const char *chat, const char *from_id, const char *cb_id, const char *data)
{
    (void)cb_id;
    if (!s_resp_buf) return;

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
        require_sensitive_action(chat, from_id, ACTION_OTA, -1);
    } else if (strcmp(data, "cmd_reboot") == 0) {
        require_sensitive_action(chat, from_id, ACTION_REBOOT_MENU, -1);
    } else if (strcmp(data, "cmd_techs") == 0) {
        tg_build_techs(s_resp_buf, RESP_BUF_SZ);
        tg_send_kb(chat, s_resp_buf, TG_KB_TECHS);
    } else if (strcmp(data, "confirm_reboot") == 0) {
        require_sensitive_action(chat, from_id, ACTION_REBOOT_NOW, -1);
    } else if (strcmp(data, "cancel_reboot") == 0) {
        snprintf(s_resp_buf, RESP_BUF_SZ, "\xf0\x9f\x8f\xad <b>ESP-IDMS</b>\nReboot cancelled. Choose:");
        tg_send_kb(chat, s_resp_buf, TG_KB_MAIN);
    } else if (strcmp(data, "ota_status") == 0) {
        require_sensitive_action(chat, from_id, ACTION_OTA, -1);
    } else if (strcmp(data, "ota_url") == 0) {
        require_sensitive_action(chat, from_id, ACTION_OTA, -1);
    } else if (strcmp(data, "tech_list") == 0) {
        tg_build_techs(s_resp_buf, RESP_BUF_SZ);
        tg_send_kb(chat, s_resp_buf, TG_KB_TECHS);
    } else if (strcmp(data, "tech_remove") == 0) {
        require_sensitive_action(chat, from_id, ACTION_REMOVE_MENU, -1);
    } else if (strncmp(data, "rm_", 3) == 0) {
        int idx = atoi(data + 3);
        require_sensitive_action(chat, from_id, ACTION_REMOVE_INDEX, idx);
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

static bool valid_bot_username(const char *username)
{
    if (!username || username[0] == '\0') {
        return false;
    }
    for (const char *p = username; *p; p++) {
        if (!isalnum((unsigned char)*p) && *p != '_') {
            return false;
        }
    }
    return true;
}

static void refresh_bot_qr_code(void)
{
    if (s_bot_qr_refreshed) {
        return;
    }

    char resp[768] = {0};
    int st = 0;
    esp_err_t err = tg_http_get("/getMe", resp, sizeof(resp), &st);
    if (err != ESP_OK || st < 200 || st >= 300) {
        ESP_LOGW(TAG, "Could not refresh Telegram QR URL from getMe: err=%s st=%d",
                 esp_err_to_name(err), st);
        return;
    }

    cJSON *root = cJSON_Parse(resp);
    if (!root) {
        ESP_LOGW(TAG, "Could not refresh Telegram QR URL: invalid getMe JSON");
        return;
    }

    const char *username = NULL;
    cJSON *ok = cJSON_GetObjectItem(root, "ok");
    cJSON *result = cJSON_GetObjectItem(root, "result");
    cJSON *user = result ? cJSON_GetObjectItem(result, "username") : NULL;
    if (cJSON_IsTrue(ok) && cJSON_IsString(user) && valid_bot_username(user->valuestring)) {
        username = user->valuestring;
    }

    if (username) {
        char url[CONFIG_INFO_STRING_MAX_LEN + 1];
        snprintf(url, sizeof(url), "https://t.me/%s", username);
        char current[CONFIG_INFO_STRING_MAX_LEN + 1] = {0};
        config_get_qr_code(current, sizeof(current));
        if (strcmp(current, url) != 0) {
            esp_err_t set_err = config_set_qr_code(url);
            if (set_err == ESP_OK) {
                ESP_LOGI(TAG, "Telegram QR URL refreshed from bot username");
                s_bot_qr_refreshed = true;
            } else {
                ESP_LOGW(TAG, "Could not store Telegram QR URL: %s", esp_err_to_name(set_err));
            }
        } else {
            s_bot_qr_refreshed = true;
        }
    }

    cJSON_Delete(root);
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
        ESP_LOGI(TAG, "Bot token configured (%zu chars)", tlen);
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
            refresh_bot_qr_code();
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
                poll_int = 0;
            } else {
                poll_int = POLL_OK_S;
            }
            continue;
        }
        poll_int = 0;

        const char *log_text = session_waits_for_secret(update.from_id) ? "<redacted>" : update.message_text;
        ESP_LOGI(TAG, "Update %d: msg=%d cb=%d from=%s chat=%s text='%.40s'",
                 update.update_id, update.is_message, update.is_callback,
                 update.from_id, update.chat_id, log_text);

        if (update.update_id > 0) {
            s_update_offset = update.update_id + 1;
            save_state();
        }

        if (update.chat_id[0] == '\0') {
            ESP_LOGW(TAG, "No chat_id in update %d", update.update_id);
            continue;
        }

        if (update.is_callback && update.callback_id[0]) {
            esp_err_t cb_err = tg_answer_cb(update.callback_id);
            if (cb_err != ESP_OK) {
                ESP_LOGD(TAG, "answerCallbackQuery failed for update %d: %s",
                         update.update_id, esp_err_to_name(cb_err));
            }
        }

        if (!is_private_chat(&update)) {
            ESP_LOGW(TAG, "Ignoring non-private Telegram update %d from=%s chat=%s type=%s",
                     update.update_id, update.from_id, update.chat_id,
                     update.chat_type[0] ? update.chat_type : "(missing)");
            if (update.is_message) {
                tg_send_text(update.chat_id,
                    "\xe2\x9d\x8c ESP-IDMS bot commands are only available in a private chat.");
            }
            continue;
        }

        if (callback_seen_recently(&update)) {
            ESP_LOGI(TAG, "Ignoring repeated callback within %d ms: from=%s data=%s",
                     CALLBACK_DEDUPE_MS, update.from_id, update.callback_data);
            continue;
        }

        if (handle_session_message(&update)) {
            continue;
        }

        if (update.has_contact && handle_contact_authorization(&update)) {
            continue;
        }

        if (!tg_is_authorized_id(update.from_id)) {
            uint8_t tech_count = config_get_tech_count();
            if (update.is_message) {
                if (has_pending_phone_slots()) {
                    request_contact_authorization(update.chat_id);
                    continue;
                }
                begin_shared_login(&update);
                continue;
            }

            ESP_LOGW(TAG, "Unauthorized %s (from_id=%s, techs=%u) — ignored",
                     update.is_callback ? "callback" : "message",
                     update.from_id, tech_count);
            snprintf(s_resp_buf, RESP_BUF_SZ,
                "\xe2\x9d\x8c <b>Access Denied</b>\n\n"
                "Your Telegram ID: <code>%s</code>\n"
                "You are not registered as a technician.\n"
                "Send /start to enter the shared admin name and password.",
                update.from_id);
            tg_send_text(update.chat_id, s_resp_buf);
            continue;
        }

        if (update.is_callback) {
            ESP_LOGI(TAG, "Callback: %s", update.callback_data);
            handle_cb(update.chat_id, update.from_id, update.callback_id, update.callback_data);
        } else if (update.is_message) {
            if (tg_is_cmd_text(update.message_text, "start") || tg_is_cmd_text(update.message_text, "help"))
                handle_cmd(update.chat_id, update.from_id, "start");
            else if (tg_is_cmd_text(update.message_text, "status"))
                handle_cmd(update.chat_id, update.from_id, "status");
            else if (tg_is_cmd_text(update.message_text, "weekly"))
                handle_cmd(update.chat_id, update.from_id, "weekly");
            else if (tg_is_cmd_text(update.message_text, "export"))
                handle_cmd(update.chat_id, update.from_id, "export");
            else if (tg_is_cmd_text(update.message_text, "test"))
                handle_cmd(update.chat_id, update.from_id, "test");
            else if (tg_is_cmd_text(update.message_text, "ota"))
                handle_cmd(update.chat_id, update.from_id, "ota");
            else if (tg_is_cmd_text(update.message_text, "reboot"))
                handle_cmd(update.chat_id, update.from_id, "reboot");
            else if (tg_is_cmd_text(update.message_text, "techs"))
                handle_cmd(update.chat_id, update.from_id, "techs");
            else if (tg_is_cmd_text(update.message_text, "remove_tech"))
                handle_cmd(update.chat_id, update.from_id,
                           update.message_text[0] == '/' ? update.message_text + 1 : update.message_text);
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
