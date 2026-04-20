#include "ds18b20.h"
#include "onewire.h"
#include "driver/gpio.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "ds18b20";

#define MAX_DEVICES 2

static uint64_t s_rom[MAX_DEVICES];
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

static void ow_match_rom(uint64_t rom)
{
    ow_write_byte(0x55);
    for (int i = 0; i < 8; i++) {
        ow_write_byte((uint8_t)((rom >> (8 * i)) & 0xFF));
    }
}

void ds18b20_init(void)
{
    ow_init((gpio_num_t)CONFIG_IDMS_PIN_ONEWIRE);
    s_count = 0;
    memset(s_rom, 0, sizeof(s_rom));

    ow_search_reset_state();
    uint64_t code = 0;
    while (s_count < MAX_DEVICES && ow_search_next(&code)) {
        s_rom[s_count++] = code;
    }
    ow_search_reset_state();

    qsort(s_rom, (size_t)s_count, sizeof(uint64_t), cmp_u64);

    ESP_LOGI(TAG, "Found %d DS18B20 device(s)", s_count);
    for (int i = 0; i < s_count; i++) {
        ESP_LOGI(TAG, " ROM[%d] = 0x%016llX", i, (unsigned long long)s_rom[i]);
    }
}

int ds18b20_device_count(void)
{
    return s_count;
}

void ds18b20_request_conversion(void)
{
    if (s_count == 0) {
        return;
    }
    if (!ow_reset()) {
        return;
    }
    ow_write_byte(0xCC);
    ow_write_byte(0x44);
}

bool ds18b20_read_temperature_c(int index, float *out_c)
{
    if (!out_c || index < 0 || index >= s_count) {
        return false;
    }
    if (!ow_reset()) {
        return false;
    }
    ow_match_rom(s_rom[index]);
    ow_write_byte(0xBE);
    uint8_t sp[9];
    for (int i = 0; i < 9; i++) {
        sp[i] = ow_read_byte();
    }
    if (ow_crc8(sp, 8) != sp[8]) {
        return false;
    }
    int16_t raw = (int16_t)(sp[0] | (sp[1] << 8));
    *out_c = (float)raw / 16.0f;
    return true;
}
