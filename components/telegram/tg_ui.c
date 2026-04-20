#include "tg_ui.h"
#include "config_store.h"
#include "monitor.h"
#include "ota.h"
#include "wifi_manager.h"
#include <stdio.h>
#include <string.h>

void tg_build_status(char *buf, size_t sz)
{
    idms_metrics_t m;
    monitor_get_metrics(&m);
    char a[32], ti[32], to[32], dt[32];
    snprintf(a,  sizeof(a),  m.current_valid ? "%.2f A" : "\xe2\x80\x94", m.current_a);
    snprintf(ti, sizeof(ti), m.t_in_valid  ? "%.1f \xc2\xb0" "C" : "\xe2\x80\x94", m.t_in_c);
    snprintf(to, sizeof(to), m.t_out_valid ? "%.1f \xc2\xb0" "C" : "\xe2\x80\x94", m.t_out_c);
    snprintf(dt, sizeof(dt), m.delta_valid  ? "%.1f \xc2\xb0" "C" : "\xe2\x80\x94", m.delta_t_c);

    snprintf(buf, sz,
        "<b>\xf0\x9f\x93\x8a ESP-IDMS Status Report</b>\n\n"
        "<b>Firmware:</b> %s\n"
        "<b>OTA:</b> %s (%s)\n\n"
        "<b>Wi-Fi:</b> %s (%s)\n\n"
        "<b>Current:</b> %s\n"
        "<b>T_in:</b> %s\n"
        "<b>T_out:</b> %s\n"
        "<b>\xce\x94T:</b> %s\n\n"
        "<b>Technicians:</b> %u/5",
        ota_get_version(),
        ota_get_status(), ota_get_partition(),
        m.wifi_connected ? "\xe2\x9c\x85" : "\xe2\x9d\x8c", m.wifi_ip[0] ? m.wifi_ip : "N/A",
        a, ti, to, dt, config_get_tech_count());
}

void tg_build_ota(char *buf, size_t sz)
{
    char ip[16];
    wifi_manager_get_ip(ip, sizeof(ip));

    char token[17] = {0};
    bool has_token = ota_generate_token(token, sizeof(token));

    if (has_token) {
        snprintf(buf, sz,
            "<b>\xf0\x9f\x9b\x9c OTA Firmware Update</b>\n\n"
            "<b>Current:</b> %s\n"
            "<b>Partition:</b> %s\n"
            "<b>Status:</b> %s\n\n"
            "Upload firmware at:\n"
            "<code>http://%s:%d/?token=%s</code>\n\n"
            "<i>Token expires in 5 minutes. Use it in the browser URL above.\n"
            "Alternatively use HTTP Basic Auth via serial console credentials.</i>",
            ota_get_version(), ota_get_partition(), ota_get_status(),
            ip[0] ? ip : "?.?.?.?", CONFIG_IDMS_OTA_HTTP_PORT,
            token);
    } else {
        snprintf(buf, sz,
            "<b>\xf0\x9f\x9b\x9c OTA Firmware Update</b>\n\n"
            "<b>Current:</b> %s\n"
            "<b>Partition:</b> %s\n"
            "<b>Status:</b> %s\n\n"
            "<i>Failed to generate access token. Use serial console to set OTA credentials, then access http://%s:%d/</i>",
            ota_get_version(), ota_get_partition(), ota_get_status(),
            ip[0] ? ip : "?.?.?.?", CONFIG_IDMS_OTA_HTTP_PORT);
    }
}

void tg_build_techs(char *buf, size_t sz)
{
    uint8_t count = config_get_tech_count();
    char list[512] = "";
    char id[64];
    for (int i = 0; i < count; i++) {
        if (config_get_tech_id(i, id, sizeof(id)) == ESP_OK) {
            char e[128];
            snprintf(e, sizeof(e), "  [%d] <code>%s</code>\n", i, id);
            size_t cur = strlen(list);
            size_t elen = strlen(e);
            if (cur + elen < sizeof(list) - 1) {
                memcpy(list + cur, e, elen + 1);
            }
        }
    }
    if (count == 0) snprintf(list, sizeof(list), "  (none)\n");
    snprintf(buf, sz,
        "<b>\xf0\x9f\x93\x8b Technician IDs</b>\n\n"
        "Registered (%u/5):\n%s\n"
        "<i>Use serial console or /remove_tech to manage.</i>",
        count, list);
}

void tg_build_reboot_confirm(char *buf, size_t sz)
{
    snprintf(buf, sz,
        "<b>\xe2\x9a\xa0\xef\xb8\x8f Confirm Reboot</b>\n\n"
        "Are you sure you want to reboot the device?\n"
        "Monitoring will be interrupted during restart.");
}

void tg_build_tech_remove(char *buf, size_t sz)
{
    uint8_t count = config_get_tech_count();
    if (count == 0) {
        snprintf(buf, sz, "<b>\xf0\x9f\x93\x8b Technician IDs</b>\n\nNo technicians registered.");
        return;
    }
    char list[512] = "";
    char id[64];
    for (int i = 0; i < count; i++) {
        if (config_get_tech_id(i, id, sizeof(id)) == ESP_OK) {
            char e[128];
            snprintf(e, sizeof(e), "  [%d] <code>%s</code>\n", i, id);
            size_t cur = strlen(list);
            size_t elen = strlen(e);
            if (cur + elen < sizeof(list) - 1) {
                memcpy(list + cur, e, elen + 1);
            }
        }
    }
    snprintf(buf, sz,
        "<b>\xe2\x9d\x8c Remove Technician</b>\n\n"
        "Select a technician to remove:\n%s",
        list);
}
