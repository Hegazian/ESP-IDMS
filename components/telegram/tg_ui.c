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
    snprintf(a,  sizeof(a),  m.current_valid ? "%.2f A" : "—", m.current_a);
    snprintf(ti, sizeof(ti), m.t_in_valid  ? "%.1f °C" : "—", m.t_in_c);
    snprintf(to, sizeof(to), m.t_out_valid ? "%.1f °C" : "—", m.t_out_c);
    snprintf(dt, sizeof(dt), m.delta_valid  ? "%.1f °C" : "—", m.delta_t_c);

    snprintf(buf, sz,
        "<b>📊 ESP-IDMS Status Report</b>\n\n"
        "<b>Firmware:</b> %s\n"
        "<b>OTA:</b> %s (%s)\n\n"
        "<b>Wi-Fi:</b> %s (%s)\n\n"
        "<b>Current:</b> %s\n"
        "<b>T_in:</b> %s\n"
        "<b>T_out:</b> %s\n"
        "<b>ΔT:</b> %s\n\n"
        "<b>Technicians:</b> %u/5",
        ota_get_version(),
        ota_get_status(), ota_get_partition(),
        m.wifi_connected ? "✅" : "❌", m.wifi_ip[0] ? m.wifi_ip : "N/A",
        a, ti, to, dt, config_get_tech_count());
}

void tg_build_ota(char *buf, size_t sz)
{
    char ip[16];
    wifi_manager_get_ip(ip, sizeof(ip));
    char ota_user[64], ota_pass[64];
    config_get_ota_user(ota_user, sizeof(ota_user));
    config_get_ota_pass(ota_pass, sizeof(ota_pass));
    snprintf(buf, sz,
        "<b>🔄 OTA Firmware Update</b>\n\n"
        "<b>Current:</b> %s\n"
        "<b>Partition:</b> %s\n"
        "<b>Status:</b> %s\n\n"
        "Upload firmware at:\n"
        "<code>http://%s:%d/</code>\n"
        "User: <code>%s</code>\n"
        "Pass: <code>%s</code>",
        ota_get_version(), ota_get_partition(), ota_get_status(),
        ip[0] ? ip : "?.?.?.?",
        CONFIG_IDMS_OTA_HTTP_PORT,
        ota_user, ota_pass);
}

void tg_build_techs(char *buf, size_t sz)
{
    uint8_t count = config_get_tech_count();
    char list[256] = "";
    char id[64];
    for (int i = 0; i < count; i++) {
        if (config_get_tech_id(i, id, sizeof(id)) == ESP_OK) {
            char e[80];
            snprintf(e, sizeof(e), "  [%d] %s\n", i, id);
            strncat(list, e, sizeof(list) - strlen(list) - 1);
        }
    }
    if (count == 0) snprintf(list, sizeof(list), "  (none)\n");
    snprintf(buf, sz,
        "<b>📋 Technician IDs</b>\n\n"
        "Registered (%u/5):\n%s\n"
        "<i>Use serial console or LCD to add more.</i>",
        count, list);
}
