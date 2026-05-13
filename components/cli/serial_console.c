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
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

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
        char id[64] = {0};
        char name[CONFIG_TECH_NAME_MAX_LEN + 1] = {0};
        int consumed = 0;
        if (sscanf(id_str, "%63s%n", id, &consumed) != 1) {
            ESP_LOGW(TAG, "Usage: add <chat_id> [name]");
            return;
        }
        const char *name_arg = id_str + consumed;
        while (*name_arg && isspace((unsigned char)*name_arg)) {
            name_arg++;
        }
        if (*name_arg) {
            strncpy(name, name_arg, sizeof(name) - 1);
            name[sizeof(name) - 1] = '\0';
        }
        esp_err_t err = config_add_tech(id, name);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Added technician ID: %s%s%s (count: %u)",
                     id, name[0] ? " name=" : "", name[0] ? name : "", config_get_tech_count());
        } else {
            ESP_LOGW(TAG, "Failed to add: %s (err=%s)", id, esp_err_to_name(err));
        }
    } else if (strcmp(cmd, "list") == 0) {
        uint8_t count = config_get_tech_count();
        ESP_LOGI(TAG, "Technician IDs (%u/%d):", count, CONFIG_TECH_MAX_COUNT);
        for (int i = 0; i < count; i++) {
            char id[64];
            char name[CONFIG_TECH_NAME_MAX_LEN + 1] = "";
            if (config_get_tech_id(i, id, sizeof(id)) == ESP_OK) {
                config_get_tech_name(i, name, sizeof(name));
                ESP_LOGI(TAG, "  [%d] %s%s%s", i,
                         name[0] ? name : "", name[0] ? " " : "", id);
            }
        }
    } else if (strncmp(cmd, "remove ", 7) == 0) {
        int idx = atoi(cmd + 7);
        if (idx < 0 || idx >= CONFIG_TECH_MAX_COUNT) {
            ESP_LOGW(TAG, "Invalid index: %d (must be 0-%d)", idx, CONFIG_TECH_MAX_COUNT - 1);
            return;
        }
        esp_err_t err = config_remove_tech(idx);
        ESP_LOGI(TAG, "Remove index %d: %s (count now %u)",
                 idx, esp_err_to_name(err), config_get_tech_count());
    } else if (strcmp(cmd, "clear") == 0) {
        esp_err_t err = config_clear_techs();
        ESP_LOGI(TAG, "Cleared all technician IDs: %s", esp_err_to_name(err));
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
        ESP_LOGI(TAG, "Faults: power=%s cooling=%s delta_alert=%s",
                 m.power_fault ? "yes" : "no",
                 m.cooling_fault ? "yes" : "no",
                 m.delta_alert ? "yes" : "no");
        ESP_LOGI(TAG, "Thresholds: power_loss=%.3f A machine_running=%.3f A current_limit=%u-%u A",
                 (double)config_get_power_loss_current_ma() / 1000.0,
                 (double)config_get_machine_running_current_ma() / 1000.0,
                 config_get_min_current(), config_get_max_current());
        ESP_LOGI(TAG, "Sensor status: %s (flags=0x%02lx)",
                 m.sensor_preflight_ok ? "OK" : m.sensor_status,
                 (unsigned long)m.sensor_error_flags);
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
    } else if (strncmp(cmd, "set_bot_admin ", 14) == 0) {
        const char *val = cmd + 14;
        if (strlen(val) < 1 || strlen(val) > CONFIG_TECH_NAME_MAX_LEN) {
            ESP_LOGW(TAG, "Bot admin name must be 1-%d characters", CONFIG_TECH_NAME_MAX_LEN);
        } else {
            esp_err_t err = config_set_telegram_admin_name(val);
            ESP_LOGI(TAG, "Set Telegram bot admin name: %s (%s)", val, esp_err_to_name(err));
        }
    } else if (strncmp(cmd, "set_bot_password ", 17) == 0) {
        const char *val = cmd + 17;
        if (strlen(val) < 6 || strlen(val) > CONFIG_TECH_PASSWORD_MAX_LEN) {
            ESP_LOGW(TAG, "Bot password must be 6-%d characters", CONFIG_TECH_PASSWORD_MAX_LEN);
        } else {
            esp_err_t err = config_set_telegram_admin_password(val);
            ESP_LOGI(TAG, "Set Telegram bot password: **** (%s)", esp_err_to_name(err));
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
    } else if (strncmp(cmd, "set_cloud_url ", 14) == 0) {
        const char *val = cmd + 14;
        if (val[0] == '\0') {
            ESP_LOGW(TAG, "Cloud URL cannot be empty");
        } else if (strncmp(val, "https://", 8) != 0
#if CONFIG_IDMS_CLOUD_ALLOW_INSECURE_HTTP
                   && strncmp(val, "http://", 7) != 0
#endif
        ) {
#if CONFIG_IDMS_CLOUD_ALLOW_INSECURE_HTTP
            ESP_LOGW(TAG, "Cloud URL must start with http:// or https://");
#else
            ESP_LOGW(TAG, "Cloud URL must start with https:// (enable insecure HTTP only for development)");
#endif
        } else {
            esp_err_t err = config_set_cloud_url(val);
            ESP_LOGI(TAG, "Set cloud URL: %s (%s)", val, esp_err_to_name(err));
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Cloud sync will use the new URL on the next upload attempt");
            }
        }
    } else if (strncmp(cmd, "set_cloud_token ", 16) == 0) {
        const char *val = cmd + 16;
        esp_err_t err = config_set_cloud_token(val);
        ESP_LOGI(TAG, "Set cloud token: %s (%s)", val[0] ? "****" : "(empty)", esp_err_to_name(err));
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Cloud sync will use the new token on the next upload attempt");
        }
    } else if (strncmp(cmd, "set_power_threshold ", 20) == 0) {
        float actual_a = strtof(cmd + 20, NULL);
        if (actual_a < 0.0f || actual_a > (float)CONFIG_CURRENT_MAX_LIMIT) {
            ESP_LOGW(TAG, "Usage: set_power_threshold <A> (0.00 to %d.00)", CONFIG_CURRENT_MAX_LIMIT);
        } else {
            uint16_t ma = (uint16_t)lroundf(actual_a * 1000.0f);
            esp_err_t err = config_set_power_loss_current_ma(ma);
            ESP_LOGI(TAG, "Set power-loss threshold: %.3f A (%s)",
                     (double)ma / 1000.0, esp_err_to_name(err));
        }
    } else if (strncmp(cmd, "set_running_threshold ", 22) == 0) {
        float actual_a = strtof(cmd + 22, NULL);
        if (actual_a < 0.0f || actual_a > (float)CONFIG_CURRENT_MAX_LIMIT) {
            ESP_LOGW(TAG, "Usage: set_running_threshold <A> (0.00 to %d.00)", CONFIG_CURRENT_MAX_LIMIT);
        } else {
            uint16_t ma = (uint16_t)lroundf(actual_a * 1000.0f);
            esp_err_t err = config_set_machine_running_current_ma(ma);
            ESP_LOGI(TAG, "Set machine-running threshold: %.3f A (%s)",
                     (double)ma / 1000.0, esp_err_to_name(err));
        }
    } else if (strcmp(cmd, "show_secrets") == 0) {
        char ssid[64], ota_user[64], cloud_url[256], cloud_token[128], bot_admin[CONFIG_TECH_NAME_MAX_LEN + 1];
        config_get_wifi_ssid(ssid, sizeof(ssid));
        config_get_ota_user(ota_user, sizeof(ota_user));
        config_get_cloud_url(cloud_url, sizeof(cloud_url));
        config_get_cloud_token(cloud_token, sizeof(cloud_token));
        config_get_telegram_admin_name(bot_admin, sizeof(bot_admin));
        ESP_LOGI(TAG, "Wi-Fi SSID:     %s", ssid[0] ? ssid : "(not set)");
        ESP_LOGI(TAG, "Wi-Fi password: ****");
        ESP_LOGI(TAG, "Telegram token: ****");
        ESP_LOGI(TAG, "Bot admin name: %s", bot_admin[0] ? bot_admin : "(not set)");
        ESP_LOGI(TAG, "Bot password:   %s", config_has_telegram_admin_password() ? "****" : "(not set)");
        ESP_LOGI(TAG, "OTA user:       %s", ota_user[0] ? ota_user : "(not set)");
        ESP_LOGI(TAG, "OTA password:   ****");
        ESP_LOGI(TAG, "Cloud URL:      %s", cloud_url[0] ? cloud_url : "(not set)");
        ESP_LOGI(TAG, "Cloud token:    %s", cloud_token[0] ? "****" : "(not set)");
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
    } else if (strcmp(cmd, "adc") == 0) {
        int mean = 0, rms = 0, errors = 0;
        idms_metrics_t m;
        monitor_adc_debug(&mean, &rms, &errors);
        monitor_get_metrics(&m);
        ESP_LOGI(TAG, "ADC debug: mean=%d, rms=%d, errors=%d/64 (GPIO%d, unit=1 ch=5)",
                 mean, rms, errors, CONFIG_IDMS_ADC_GPIO);
        ESP_LOGI(TAG, "  Current: %.2f A (%s), calibration %.2f A/V",
                 (double)m.current_a, m.current_valid ? "valid" : "invalid",
                 (double)config_get_current_cal_x100() / 100.0);
        ESP_LOGI(TAG, "  Expected: mean roughly mid-scale with bias circuit (about 900-3200 counts)");
        ESP_LOGI(TAG, "  If mean is near 0, TP_ADC/GPIO%d is shorted to GND or bias divider is missing", CONFIG_IDMS_ADC_GPIO);
        ESP_LOGI(TAG, "  If mean is near 4095, TP_ADC/GPIO%d is floating/high", CONFIG_IDMS_ADC_GPIO);
        ESP_LOGI(TAG, "  If errors>0: GPIO%d may not support ADC or ADC not initialized", CONFIG_IDMS_ADC_GPIO);
    } else if (strncmp(cmd, "set_current_cal ", 16) == 0) {
        float amps_per_volt = strtof(cmd + 16, NULL);
        if (amps_per_volt < 0.10f || amps_per_volt > 5000.0f) {
            ESP_LOGW(TAG, "Usage: set_current_cal <amps_per_volt> (0.10 to 5000.00)");
        } else {
            uint32_t cal_x100 = (uint32_t)lroundf(amps_per_volt * 100.0f);
            esp_err_t err = config_set_current_cal_x100(cal_x100);
            monitor_reset_current_filter();
            ESP_LOGI(TAG, "Set current calibration: %.2f A/V (%s)",
                     (double)amps_per_volt, esp_err_to_name(err));
        }
    } else if (strncmp(cmd, "cal_current ", 12) == 0) {
        float actual_a = strtof(cmd + 12, NULL);
        idms_metrics_t m;
        monitor_get_metrics(&m);
        if (!m.current_valid || m.current_a < 0.05f || actual_a <= 0.0f) {
            ESP_LOGW(TAG, "Cannot calibrate. Need valid displayed current and actual amps > 0.");
            ESP_LOGW(TAG, "Current now: %.2f A (%s)", (double)m.current_a,
                     m.current_valid ? "valid" : "invalid");
        } else {
            uint32_t old_cal = config_get_current_cal_x100();
            float new_cal_f = ((float)old_cal * actual_a) / m.current_a;
            if (new_cal_f < 10.0f || new_cal_f > 500000.0f) {
                ESP_LOGW(TAG, "Calculated calibration out of range: %.2f x100", (double)new_cal_f);
                ESP_LOGW(TAG, "Check wiring: CT must clamp around one conductor only.");
            } else {
                uint32_t new_cal = (uint32_t)lroundf(new_cal_f);
                esp_err_t err = config_set_current_cal_x100(new_cal);
                monitor_reset_current_filter();
                ESP_LOGI(TAG, "Current calibrated: measured=%.2f A actual=%.2f A scale %.2f -> %.2f A/V (%s)",
                         (double)m.current_a, (double)actual_a,
                         (double)old_cal / 100.0, (double)new_cal / 100.0,
                         esp_err_to_name(err));
            }
        }
    } else if (strcmp(cmd, "cal_zero") == 0) {
#if CONFIG_IDMS_SCT_AUTOZERO_ENABLE
        ESP_LOGI(TAG, "Current zero calibration requested. Keep the CT connected and the load OFF.");
        esp_err_t err = monitor_calibrate_zero();
        monitor_reset_current_filter();
        ESP_LOGI(TAG, "Current zero calibration result: %s", esp_err_to_name(err));
#else
        ESP_LOGW(TAG, "Current zero calibration is disabled in menuconfig");
#endif
    } else if (strcmp(cmd, "cal_all") == 0) {
        ESP_LOGI(TAG, "Running sensor self-test and safe automatic calibration");
        ESP_LOGI(TAG, "Temperature sensors are presence/range checked; absolute calibration requires a reference thermometer.");
        esp_err_t err = monitor_calibrate_all();
        ESP_LOGI(TAG, "Sensor calibration/self-test result: %s", esp_err_to_name(err));
    } else if (strncmp(cmd, "set_dt_high ", 12) == 0) {
        int value = atoi(cmd + 12);
        esp_err_t err = config_set_dt_high_threshold((int16_t)value);
        ESP_LOGI(TAG, "Set Delta-T high threshold: %d C (%s)", value, esp_err_to_name(err));
    } else if (strncmp(cmd, "topway_str ", 11) == 0) {
#if CONFIG_IDMS_DISPLAY_TOPWAY
        const char *arg = cmd + 11;
        unsigned long addr = 0;
        int consumed = 0;
        if (sscanf(arg, "%lx%n", &addr, &consumed) == 1) {
            const char *text = arg + consumed;
            while (*text == ' ') text++;
            if (strlen(text) > 0) {
                topway_str_write((uint32_t)addr, text);
                ESP_LOGI(TAG, "Wrote \"%s\" to VP 0x%06lX", text, addr);
            } else {
                ESP_LOGW(TAG, "Usage: topway_str <hex_addr> <text>");
            }
        } else {
            ESP_LOGW(TAG, "Usage: topway_str <hex_addr> <text>");
        }
#else
        ESP_LOGW(TAG, "Topway display not enabled in config");
#endif
    } else if (strncmp(cmd, "topway_usb_unlock ", 18) == 0) {
#if CONFIG_IDMS_DISPLAY_TOPWAY
        const char *password = cmd + 18;
        while (*password && isspace((unsigned char)*password)) {
            password++;
        }
        if (password[0] == '\0') {
            ESP_LOGW(TAG, "Usage: topway_usb_unlock <password>");
        } else {
            esp_err_t err = topway_usb_unlock(password);
            ESP_LOGI(TAG, "Topway USB unlock packet sent: %s", esp_err_to_name(err));
        }
#else
        ESP_LOGW(TAG, "Topway display not enabled in config");
#endif
    } else if (strcmp(cmd, "help") == 0) {
        ESP_LOGI(TAG, "Commands:");
        ESP_LOGI(TAG, "  add <chat_id> [name] - Add authorized technician ID");
        ESP_LOGI(TAG, "  list             - List registered IDs");
        ESP_LOGI(TAG, "  remove <index>   - Remove ID by index (0-%d)", CONFIG_TECH_MAX_COUNT - 1);
        ESP_LOGI(TAG, "  clear            — Remove all IDs");
        ESP_LOGI(TAG, "  status           — Show device status");
        ESP_LOGI(TAG, "  set_ssid <ssid>  — Set Wi-Fi SSID (NVS)");
        ESP_LOGI(TAG, "  set_pass <pass>  — Set Wi-Fi password (NVS)");
        ESP_LOGI(TAG, "  set_token <tok>  — Set Telegram bot token (format: 1234567890:AAHxxxxx)");
        ESP_LOGI(TAG, "  show_token       — Show current token (first/last chars only)");
        ESP_LOGI(TAG, "  set_bot_admin <name> - Set shared Telegram login admin name");
        ESP_LOGI(TAG, "  set_bot_password <p> - Set shared Telegram login password");
        ESP_LOGI(TAG, "  set_ota_user <u> — Set OTA HTTP username (NVS)");
        ESP_LOGI(TAG, "  set_ota_pass <p> — Set OTA HTTP password (NVS)");
        ESP_LOGI(TAG, "  set_cloud_url <url> — Set telemetry upload endpoint (NVS)");
        ESP_LOGI(TAG, "  set_cloud_token <t> — Set telemetry upload bearer token (NVS)");
        ESP_LOGI(TAG, "  show_secrets     — Show secret status (values hidden)");
        ESP_LOGI(TAG, "  topway_test      — Write test values to Topway screen");
        ESP_LOGI(TAG, "  topway_str <hex_addr> <text> — Write string to Topway VP address");
        ESP_LOGI(TAG, "  topway_usb_unlock <p> - Unlock Topway USB drive access");
        ESP_LOGI(TAG, "  adc              - Read raw ADC values (diagnose current sensor)");
        ESP_LOGI(TAG, "  cal_zero         - Safe no-load current zero calibration");
        ESP_LOGI(TAG, "  cal_all          - Sensor self-test + safe automatic calibration");
        ESP_LOGI(TAG, "  cal_current <A>  - Calibrate current using a known load");
        ESP_LOGI(TAG, "  set_current_cal <A/V> - Set current calibration scale directly");
        ESP_LOGI(TAG, "  set_power_threshold <A> - Current below this means power loss");
        ESP_LOGI(TAG, "  set_running_threshold <A> - Current above this enables cooling alerts");
        ESP_LOGI(TAG, "  set_dt_high <C>  - Set Delta-T high threshold");
        ESP_LOGI(TAG, "  reboot           - Reboot device");
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
