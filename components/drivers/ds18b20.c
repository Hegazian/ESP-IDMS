#include "ds18b20.h"
#include "onewire.h"
#include "driver/gpio.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "ds18b20";

#define MAX_DEVICES 2
#define DS18B20_READ_RETRIES 3

static ow_bus_t s_buses[MAX_DEVICES];
static int s_num_buses = 0;
static uint64_t s_rom[MAX_DEVICES];
static int s_rom_bus[MAX_DEVICES];
static bool s_present[MAX_DEVICES];
static int s_count = 0;

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    if (x < y) {
        return -1;
    }
    if (x > y) {
        return 1;
    }
    return 0;
}

static void ow_match_rom(ow_bus_t *bus, uint64_t rom)
{
    ow_bus_write_byte(bus, 0x55);
    for (int i = 0; i < 8; i++) {
        ow_bus_write_byte(bus, (uint8_t)((rom >> (8 * i)) & 0xFF));
    }
}

void ds18b20_init(void)
{
    s_count = 0;
    s_num_buses = 0;
    memset(s_rom, 0, sizeof(s_rom));
    memset(s_rom_bus, 0, sizeof(s_rom_bus));
    memset(s_present, 0, sizeof(s_present));

    gpio_num_t pin1 = (gpio_num_t)CONFIG_IDMS_PIN_ONEWIRE;
    gpio_num_t pin2 = (gpio_num_t)CONFIG_IDMS_PIN_ONEWIRE2;

    ow_bus_init(&s_buses[0], pin1);
    s_num_buses = 1;

    if (pin2 != pin1) {
        ow_bus_init(&s_buses[1], pin2);
        s_num_buses = 2;
    } else {
        ESP_LOGW(TAG, "T_out pin (%d) is same as T_in — falling back to single bus", (int)pin2);
    }

    /* Search each bus for devices. With two dedicated buses, index 0 is
     * always T_in/GPIO4 and index 1 is always T_out/GPIO15.
     */
    for (int b = 0; b < s_num_buses && s_count < MAX_DEVICES; b++) {
        bool present = ow_bus_reset(&s_buses[b]);
        ESP_LOGI(TAG, "Bus %d (GPIO%d) reset: %s", b, (int)(b == 0 ? pin1 : pin2),
                 present ? "PRESENT pulse detected" : "NO presence pulse");

        if (!present) continue;

        ow_bus_search_reset_state(&s_buses[b]);
        uint64_t code = 0;
        if (s_num_buses == 1) {
            while (s_count < MAX_DEVICES && ow_bus_search_next(&s_buses[b], &code)) {
                s_rom[s_count] = code;
                s_rom_bus[s_count] = b;
                s_present[s_count] = true;
                s_count++;
            }
        } else if (ow_bus_search_next(&s_buses[b], &code)) {
            s_rom[b] = code;
            s_rom_bus[b] = b;
            s_present[b] = true;
            s_count++;
            if (ow_bus_search_next(&s_buses[b], &code)) {
                ESP_LOGW(TAG, "Bus %d has more than one device; using first ROM only", b);
            }
        }
        ow_bus_search_reset_state(&s_buses[b]);
    }

    /* For a shared bus, sort ROMs so ordering is stable across reboots */
    if (s_num_buses == 1 && s_count > 1) {
        qsort(s_rom, (size_t)s_count, sizeof(uint64_t), cmp_u64);
    }

    ESP_LOGI(TAG, "Found %d DS18B20 device(s) on %d bus(es)", s_count, s_num_buses);
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (!s_present[i]) {
            continue;
        }
        ESP_LOGI(TAG, " ROM[%d] = 0x%016llX  (bus %d)", i,
                 (unsigned long long)s_rom[i], s_rom_bus[i]);
    }
}

int ds18b20_device_count(void)
{
    return s_count;
}

bool ds18b20_sensor_present(int index)
{
    return index >= 0 && index < MAX_DEVICES && s_present[index];
}

void ds18b20_request_conversion(void)
{
    if (s_count == 0) {
        return;
    }
    for (int b = 0; b < s_num_buses; b++) {
        if (!ow_bus_reset(&s_buses[b])) {
            continue;
        }
        ow_bus_write_byte(&s_buses[b], 0xCC);
        ow_bus_write_byte(&s_buses[b], 0x44);
    }
}

static bool ds18b20_read_temperature_once(int index, float *out_c, uint8_t *scratchpad)
{
    if (!out_c || index < 0 || index >= MAX_DEVICES || !s_present[index]) {
        return false;
    }

    ow_bus_t *bus = &s_buses[s_rom_bus[index]];

    if (!ow_bus_reset(bus)) {
        ESP_LOGW(TAG, "Sensor %d reset failed", index);
        return false;
    }
    ow_match_rom(bus, s_rom[index]);
    ow_bus_write_byte(bus, 0xBE);
    uint8_t sp[9];
    for (int i = 0; i < 9; i++) {
        sp[i] = ow_bus_read_byte(bus);
    }
    if (scratchpad) {
        memcpy(scratchpad, sp, sizeof(sp));
    }
    uint8_t crc = ow_crc8(sp, 8);
    if (crc != sp[8]) {
        return false;
    }
    int16_t raw = (int16_t)(sp[0] | (sp[1] << 8));
    *out_c = (float)raw / 16.0f;
    return true;
}

bool ds18b20_read_temperature_c(int index, float *out_c)
{
    uint8_t last_sp[9] = {0};

    for (int attempt = 1; attempt <= DS18B20_READ_RETRIES; attempt++) {
        if (ds18b20_read_temperature_once(index, out_c, last_sp)) {
            return true;
        }
        if (attempt < DS18B20_READ_RETRIES) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

    uint8_t crc = ow_crc8(last_sp, 8);
    ESP_LOGW(TAG, "Sensor %d read failed after %d attempts: %02X %02X %02X %02X %02X %02X %02X %02X %02X calc=%02X",
             index, DS18B20_READ_RETRIES,
             last_sp[0], last_sp[1], last_sp[2], last_sp[3], last_sp[4],
             last_sp[5], last_sp[6], last_sp[7], last_sp[8], crc);
    return false;
}
