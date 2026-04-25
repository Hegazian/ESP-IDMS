#include "serial_console.h"
#include "config_store.h"
#include "ota.h"
#include "monitor.h"
#include "wifi_manager.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#if CONFIG_IDMS_DISPLAY_TOPWAY
#include "topway_lcd.h"
#endif

static const char *TAG = "console";

#define CMD_BUF_SIZE 256

static void handle_cmd(const char *cmd)
{
    /* Trim leading whitespace */
    while (*cmd && isspace((unsigned char)*cmd)) {
        cmd++;
    }
    if (*cmd == '\0') {
        return;
    }

    if (strncmp(cmd, "add ", 4) == 0) {
        const char *id_str = cmd + 4;
        esp_err_t err = config_add_tech_id(id_str);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Added technician ID: %s (count: %u)", id_str, config_get_tech_count());
        } else {
            ESP_LOGW(TAG, "Failed to add: %s (err=%s)", id_str, esp_err_to_name(err));
        }
    } else if (strcmp(cmd, "list") == 0) {
        uint8_t count = config_get_tech_count();
        ESP_LOGI(TAG, "Technician IDs (%u/5):", count);
        for (int i = 0; i < count; i++) {
            char id[64];
            if (config_get_tech_id(i, id, sizeof(id)) == ESP_OK) {
                ESP_LOGI(TAG, "  [%d] %s", i, id);
            }
        }
    } else if (strncmp(cmd, "remove ", 7) == 0) {
        int idx = atoi(cmd + 7);
        if (idx < 0 || idx >= 5) {
            ESP_LOGW(TAG, "Invalid index: %d (must be 0-4)", idx);
            return;
        }
        /* Shift remaining IDs down */
        uint8_t count = config_get_tech_count();
        if (idx >= count) {
            ESP_LOGW(TAG, "Index %d out of range (count=%u)", idx, count);
            return;
        }
        /* Remove: shift IDs above idx down by one */
        nvs_handle_t h;
        if (nvs_open("idms", NVS_READWRITE, &h) != ESP_OK) {
            ESP_LOGE(TAG, "NVS open failed");
            return;
        }
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
        /* Delete the last key */
        char last_key[16];
        snprintf(last_key, sizeof(last_key), "tech_id_%d", count - 1);
        nvs_erase_key(h, last_key);
        nvs_set_u8(h, "tech_count", count - 1);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "Removed index %d (count now %u)", idx, count - 1);
    } else if (strcmp(cmd, "clear") == 0) {
        nvs_handle_t h;
        if (nvs_open("idms", NVS_READWRITE, &h) != ESP_OK) {
            ESP_LOGE(TAG, "NVS open failed");
            return;
        }
        uint8_t count = config_get_tech_count();
        for (int i = 0; i < count; i++) {
            char key[16];
            snprintf(key, sizeof(key), "tech_id_%d", i);
            nvs_erase_key(h, key);
        }
        nvs_set_u8(h, "tech_count", 0);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "Cleared all technician IDs");
    } else if (strcmp(cmd, "status") == 0) {
        idms_metrics_t m;
        monitor_get_metrics(&m);
        ESP_LOGI(TAG, "=== ESP-IDMS Status ===");
        ESP_LOGI(TAG, "Firmware: %s", ota_get_version());
        ESP_LOGI(TAG, "OTA: %s (%s)", ota_get_status(), ota_get_partition());
        ESP_LOGI(TAG, "Wi-Fi: %s (%s)", m.wifi_connected ? "connected" : "offline", m.wifi_ip);
        ESP_LOGI(TAG, "Current: %.2f A (%s)", m.current_a, m.current_valid ? "valid" : "invalid");
        ESP_LOGI(TAG, "T_in: %.1f C (%s)", m.t_in_c, m.t_in_valid ? "valid" : "invalid");
        ESP_LOGI(TAG, "T_out: %.1f C (%s)", m.t_out_c, m.t_out_valid ? "valid" : "invalid");
        ESP_LOGI(TAG, "Delta T: %.1f C (%s)", m.delta_t_c, m.delta_valid ? "valid" : "invalid");
        ESP_LOGI(TAG, "Technicians: %u", config_get_tech_count());
    } else if (strcmp(cmd, "reboot") == 0) {
        ESP_LOGI(TAG, "Rebooting...");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    } else if (strncmp(cmd, "set_ssid ", 9) == 0) {
        const char *val = cmd + 9;
        esp_err_t err = config_set_wifi_ssid(val);
        ESP_LOGI(TAG, "Set Wi-Fi SSID: %s (%s)", val, esp_err_to_name(err));
    } else if (strncmp(cmd, "set_pass ", 9) == 0) {
        const char *val = cmd + 9;
        esp_err_t err = config_set_wifi_password(val);
        ESP_LOGI(TAG, "Set Wi-Fi password: %s (%s)", err == ESP_OK ? "****" : val, esp_err_to_name(err));
    } else if (strncmp(cmd, "set_token ", 10) == 0) {
        const char *val = cmd + 10;
        size_t vlen = strlen(val);
        if (vlen < 20) {
            ESP_LOGW(TAG, "Token too short (%zu chars). Expected format: 1234567890:AAHxxxxxxxxxxxxxxxx", vlen);
            ESP_LOGW(TAG, "Get your token from @BotFather on Telegram");
        } else if (!strchr(val, ':')) {
            ESP_LOGW(TAG, "Invalid token format — must contain ':' (e.g. 1234567890:AAHxxxxxxxxx)");
        } else {
            esp_err_t err = config_set_telegram_token(val);
            ESP_LOGI(TAG, "Set Telegram token: **** (%s)", esp_err_to_name(err));
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Token set! Reboot to activate: type 'reboot'");
            }
        }
    } else if (strncmp(cmd, "show_token", 10) == 0) {
        char tok[128];
        config_get_telegram_token(tok, sizeof(tok));
        size_t tlen = strlen(tok);
        if (tlen == 0) {
            ESP_LOGI(TAG, "Telegram token: (not set)");
        } else if (tlen < 20) {
            ESP_LOGW(TAG, "Telegram token: %s (%zu chars — TOO SHORT, expected ~45)", tok, tlen);
        } else {
            ESP_LOGI(TAG, "Telegram token: %.5s...%s (%zu chars)", tok, &tok[tlen-3], tlen);
        }
    } else if (strncmp(cmd, "set_ota_user ", 13) == 0) {
        const char *val = cmd + 13;
        if (strlen(val) < 3) {
            ESP_LOGW(TAG, "OTA username must be at least 3 characters");
        } else {
            esp_err_t err = config_set_ota_user(val);
            ESP_LOGI(TAG, "Set OTA username: %s (%s)", val, esp_err_to_name(err));
        }
    } else if (strncmp(cmd, "set_ota_pass ", 13) == 0) {
        const char *val = cmd + 13;
        if (strlen(val) < 6) {
            ESP_LOGW(TAG, "OTA password must be at least 6 characters");
        } else {
            esp_err_t err = config_set_ota_pass(val);
            ESP_LOGI(TAG, "Set OTA password: **** (%s)", esp_err_to_name(err));
        }
    } else if (strcmp(cmd, "show_secrets") == 0) {
        char ssid[64], ota_user[64];
        config_get_wifi_ssid(ssid, sizeof(ssid));
        config_get_ota_user(ota_user, sizeof(ota_user));
        ESP_LOGI(TAG, "Wi-Fi SSID:     %s", ssid[0] ? ssid : "(not set)");
        ESP_LOGI(TAG, "Wi-Fi password: ****");
        ESP_LOGI(TAG, "Telegram token: ****");
        ESP_LOGI(TAG, "OTA user:       %s", ota_user[0] ? ota_user : "(not set)");
        ESP_LOGI(TAG, "OTA password:   ****");
    } else if (strcmp(cmd, "topway_test") == 0) {
#if CONFIG_IDMS_DISPLAY_TOPWAY
        ESP_LOGI(TAG, "Writing test values to Topway screen...");
        topway_n16_write(0x080000, 123);
        vTaskDelay(pdMS_TO_TICKS(50));
        topway_n16_write(0x080002, 456);
        vTaskDelay(pdMS_TO_TICKS(50));
        topway_n16_write(0x080004, 789);
        vTaskDelay(pdMS_TO_TICKS(50));
        topway_n16_write(0x080006, 999);
        ESP_LOGI(TAG, "Test values sent: 123, 456, 789, 999");
#else
        ESP_LOGW(TAG, "Topway display not enabled in config");
#endif
    } else if (strcmp(cmd, "help") == 0) {
        ESP_LOGI(TAG, "Commands:");
        ESP_LOGI(TAG, "  add <chat_id>    — Add technician ID to NVS");
        ESP_LOGI(TAG, "  list             — List registered IDs");
        ESP_LOGI(TAG, "  remove <index>   — Remove ID by index (0-4)");
        ESP_LOGI(TAG, "  clear            — Remove all IDs");
        ESP_LOGI(TAG, "  status           — Show device status");
        ESP_LOGI(TAG, "  set_ssid <ssid>  — Set Wi-Fi SSID (NVS)");
        ESP_LOGI(TAG, "  set_pass <pass>  — Set Wi-Fi password (NVS)");
        ESP_LOGI(TAG, "  set_token <tok>  — Set Telegram bot token (format: 1234567890:AAHxxxxx)");
        ESP_LOGI(TAG, "  show_token       — Show current token (first/last chars only)");
        ESP_LOGI(TAG, "  set_ota_user <u> — Set OTA HTTP username (NVS)");
        ESP_LOGI(TAG, "  set_ota_pass <p> — Set OTA HTTP password (NVS)");
        ESP_LOGI(TAG, "  show_secrets     — Show secret status (values hidden)");
        ESP_LOGI(TAG, "  topway_test      — Write test values to Topway screen");
        ESP_LOGI(TAG, "  reboot           — Reboot device");
        ESP_LOGI(TAG, "  help             — Show this help");
    } else {
        ESP_LOGW(TAG, "Unknown command: '%s' (type 'help' for commands)", cmd);
    }
}

static void console_task(void *arg)
{
    (void)arg;
    char buf[CMD_BUF_SIZE];
    int pos = 0;

    /* Wait a moment for UART to stabilize */
    vTaskDelay(pdMS_TO_TICKS(1000));

    printf("\n=== ESP-IDMS Serial Console ===\n");
    printf("Type 'help' for commands\n> ");
    fflush(stdout);

    for (;;) {
        int c = getchar();
        if (c == EOF) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (c == '\r' || c == '\n') {
            if (pos > 0) {
                buf[pos] = '\0';
                printf("\r\n");
                fflush(stdout);
                handle_cmd(buf);
            }
            pos = 0;
            buf[0] = '\0';
            printf("> ");
            fflush(stdout);
        } else if (c == '\b' || c == 0x7F) {
            if (pos > 0) {
                pos--;
                buf[pos] = '\0';
                printf("\b \b");
                fflush(stdout);
            }
        } else if (c == 0x03) {
            pos = 0;
            buf[0] = '\0';
            printf("^C\r\n> ");
            fflush(stdout);
        } else if (c >= 0x20 && c < 0x7F && pos < CMD_BUF_SIZE - 2) {
            buf[pos++] = (char)c;
            buf[pos] = '\0';
            putchar(c);
            fflush(stdout);
        }
    }
}

void serial_console_start(void)
{
    xTaskCreatePinnedToCore(console_task, "console", 4096, NULL, 2, NULL, 0);
    ESP_LOGI(TAG, "Serial console started");
}
