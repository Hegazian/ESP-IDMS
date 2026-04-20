/*
 * 1-Wire bit-level driver + Dallas search (adapted from Paul Stoffregen OneWire.cpp, MIT license).
 * See upstream copyright in https://github.com/PaulStoffregen/OneWire
 */
#include "onewire.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static gpio_num_t s_pin = GPIO_NUM_NC;
static portMUX_TYPE s_ow_mux = portMUX_INITIALIZER_UNLOCKED;

static void ow_drive_low(void)
{
    gpio_set_level(s_pin, 0);
}

static void ow_release(void)
{
    gpio_set_level(s_pin, 1);
}

void ow_init(gpio_num_t pin)
{
    s_pin = pin;
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << s_pin,
        .mode = GPIO_MODE_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    ow_release();
}

bool ow_reset(void)
{
    taskENTER_CRITICAL(&s_ow_mux);
    ow_drive_low();
    esp_rom_delay_us(480);
    ow_release();
    esp_rom_delay_us(70);
    int level = gpio_get_level(s_pin);
    taskEXIT_CRITICAL(&s_ow_mux);
    esp_rom_delay_us(410);
    return level == 0;
}

static void ow_write_bit(int b)
{
    taskENTER_CRITICAL(&s_ow_mux);
    if (b) {
        ow_drive_low();
        esp_rom_delay_us(6);
        ow_release();
        esp_rom_delay_us(64);
    } else {
        ow_drive_low();
        esp_rom_delay_us(60);
        ow_release();
        esp_rom_delay_us(10);
    }
    taskEXIT_CRITICAL(&s_ow_mux);
}

static int ow_read_bit(void)
{
    int v;
    taskENTER_CRITICAL(&s_ow_mux);
    ow_drive_low();
    esp_rom_delay_us(3);
    ow_release();
    esp_rom_delay_us(10);
    v = gpio_get_level(s_pin);
    taskEXIT_CRITICAL(&s_ow_mux);
    esp_rom_delay_us(53);
    return v;
}

void ow_write_byte(uint8_t v)
{
    for (int i = 0; i < 8; i++) {
        ow_write_bit(v & 1);
        v >>= 1;
    }
}

uint8_t ow_read_byte(void)
{
    uint8_t v = 0;
    for (int i = 0; i < 8; i++) {
        if (ow_read_bit()) {
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

static uint8_t s_rom_no[8];
static uint8_t s_last_discrepancy = 0;
static uint8_t s_last_family_discrepancy = 0;
static bool s_last_device_flag = false;

void ow_search_reset_state(void)
{
    s_last_discrepancy = 0;
    s_last_family_discrepancy = 0;
    s_last_device_flag = false;
    memset(s_rom_no, 0, sizeof(s_rom_no));
}

bool ow_search_next(uint64_t *rom_code)
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

    if (!s_last_device_flag) {
        if (!ow_reset()) {
            ow_search_reset_state();
            return false;
        }
        ow_write_byte(0xF0);

        do {
            id_bit = ow_read_bit();
            cmp_id_bit = ow_read_bit();

            if ((id_bit == 1) && (cmp_id_bit == 1)) {
                break;
            } else {
                if (id_bit != cmp_id_bit) {
                    search_direction = id_bit;
                } else {
                    if (id_bit_number < s_last_discrepancy) {
                        search_direction = ((s_rom_no[rom_byte_number] & rom_byte_mask) > 0);
                    } else {
                        search_direction = (id_bit_number == s_last_discrepancy);
                    }
                    if (search_direction == 0) {
                        last_zero = id_bit_number;
                        if (last_zero < 9) {
                            s_last_family_discrepancy = last_zero;
                        }
                    }
                }

                if (search_direction == 1) {
                    s_rom_no[rom_byte_number] |= rom_byte_mask;
                } else {
                    s_rom_no[rom_byte_number] &= ~rom_byte_mask;
                }

                ow_write_bit(search_direction);

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
        s_last_discrepancy = last_zero;
        if (s_last_discrepancy == 0) {
            s_last_device_flag = true;
        }
        search_result = true;
    }

    if (!search_result || !s_rom_no[0]) {
        ow_search_reset_state();
        return false;
    }

    uint64_t rom = 0;
    for (int i = 0; i < 8; i++) {
        rom |= (uint64_t)s_rom_no[i] << (8 * i);
    }
    *rom_code = rom;
    return true;
}
