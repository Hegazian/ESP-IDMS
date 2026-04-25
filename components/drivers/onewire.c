/*
 * 1-Wire bit-level driver + Dallas search (adapted from Paul Stoffregen OneWire.cpp, MIT license).
 * See upstream copyright in https://github.com/PaulStoffregen/OneWire
 *
 * Multi-bus variant: each 1-Wire bus is an independent ow_bus_t instance.
 * Uses explicit INPUT/OUTPUT direction toggling for maximum compatibility.
 */
#include "onewire.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static void ow_drive_low(ow_bus_t *bus)
{
    gpio_set_direction(bus->pin, GPIO_MODE_OUTPUT);
    gpio_set_level(bus->pin, 0);
}

static void ow_release(ow_bus_t *bus)
{
    gpio_set_direction(bus->pin, GPIO_MODE_INPUT);
    /* Internal pull-up was enabled during init; external 4.7k must also be present */
}

void ow_bus_init(ow_bus_t *bus, gpio_num_t pin)
{
    bus->pin = pin;
    bus->mux = (portMUX_TYPE) portMUX_INITIALIZER_UNLOCKED;
    bus->last_discrepancy = 0;
    bus->last_family_discrepancy = 0;
    bus->last_device_flag = false;
    memset(bus->rom_no, 0, sizeof(bus->rom_no));

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << bus->pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
}

bool ow_bus_reset(ow_bus_t *bus)
{
    taskENTER_CRITICAL(&bus->mux);
    ow_drive_low(bus);
    esp_rom_delay_us(480);
    ow_release(bus);
    esp_rom_delay_us(80);
    int level = gpio_get_level(bus->pin);
    taskEXIT_CRITICAL(&bus->mux);
    esp_rom_delay_us(400);
    return level == 0;
}

static void ow_write_bit(ow_bus_t *bus, int b)
{
    taskENTER_CRITICAL(&bus->mux);
    ow_drive_low(bus);
    if (b) {
        esp_rom_delay_us(6);
        ow_release(bus);
        esp_rom_delay_us(64);
    } else {
        esp_rom_delay_us(65);
        ow_release(bus);
        esp_rom_delay_us(10);
    }
    taskEXIT_CRITICAL(&bus->mux);
}

static int ow_read_bit(ow_bus_t *bus)
{
    int v;
    taskENTER_CRITICAL(&bus->mux);
    ow_drive_low(bus);
    esp_rom_delay_us(3);
    ow_release(bus);
    esp_rom_delay_us(17);   /* sample at ~20 us, well inside 15-60 us window */
    v = gpio_get_level(bus->pin);
    taskEXIT_CRITICAL(&bus->mux);
    esp_rom_delay_us(47);   /* complete 70 us slot */
    return v;
}

void ow_bus_write_byte(ow_bus_t *bus, uint8_t v)
{
    for (int i = 0; i < 8; i++) {
        ow_write_bit(bus, v & 1);
        v >>= 1;
    }
}

uint8_t ow_bus_read_byte(ow_bus_t *bus)
{
    uint8_t v = 0;
    for (int i = 0; i < 8; i++) {
        if (ow_read_bit(bus)) {
            v |= 1 << i;
        }
    }
    return v;
}

uint8_t ow_crc8(const uint8_t *addr, uint8_t len)
{
    uint8_t crc = 0;
    while (len--) {
        uint8_t inbyte = *addr++;
        for (uint8_t i = 8; i; i--) {
            uint8_t mix = (crc ^ inbyte) & 0x01;
            crc >>= 1;
            if (mix) {
                crc ^= 0x8C;
            }
            inbyte >>= 1;
        }
    }
    return crc;
}

void ow_bus_search_reset_state(ow_bus_t *bus)
{
    bus->last_discrepancy = 0;
    bus->last_family_discrepancy = 0;
    bus->last_device_flag = false;
    memset(bus->rom_no, 0, sizeof(bus->rom_no));
}

bool ow_bus_search_next(ow_bus_t *bus, uint64_t *rom_code)
{
    if (!rom_code) {
        return false;
    }

    uint8_t id_bit_number = 1;
    uint8_t last_zero = 0;
    uint8_t rom_byte_number = 0;
    bool search_result = false;
    uint8_t id_bit;
    uint8_t cmp_id_bit;
    unsigned char rom_byte_mask = 1;
    unsigned char search_direction;

    if (!bus->last_device_flag) {
        if (!ow_bus_reset(bus)) {
            ow_bus_search_reset_state(bus);
            return false;
        }
        ow_bus_write_byte(bus, 0xF0);

        do {
            id_bit = ow_read_bit(bus);
            cmp_id_bit = ow_read_bit(bus);

            if ((id_bit == 1) && (cmp_id_bit == 1)) {
                break;
            } else {
                if (id_bit != cmp_id_bit) {
                    search_direction = id_bit;
                } else {
                    if (id_bit_number < bus->last_discrepancy) {
                        search_direction = ((bus->rom_no[rom_byte_number] & rom_byte_mask) > 0);
                    } else {
                        search_direction = (id_bit_number == bus->last_discrepancy);
                    }
                    if (search_direction == 0) {
                        last_zero = id_bit_number;
                        if (last_zero < 9) {
                            bus->last_family_discrepancy = last_zero;
                        }
                    }
                }

                if (search_direction == 1) {
                    bus->rom_no[rom_byte_number] |= rom_byte_mask;
                } else {
                    bus->rom_no[rom_byte_number] &= ~rom_byte_mask;
                }

                ow_write_bit(bus, search_direction);

                id_bit_number++;
                rom_byte_mask <<= 1;

                if (rom_byte_mask == 0) {
                    rom_byte_number++;
                    rom_byte_mask = 1;
                }
            }
        } while (rom_byte_number < 8);
    }

    if (!(id_bit_number < 65)) {
        bus->last_discrepancy = last_zero;
        if (bus->last_discrepancy == 0) {
            bus->last_device_flag = true;
        }
        search_result = true;
    }

    if (!search_result || !bus->rom_no[0]) {
        ow_bus_search_reset_state(bus);
        return false;
    }

    uint64_t rom = 0;
    for (int i = 0; i < 8; i++) {
        rom |= (uint64_t)bus->rom_no[i] << (8 * i);
    }
    *rom_code = rom;
    return true;
}
