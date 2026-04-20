#include "sdkconfig.h"
#include "lcd_diag.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdint.h>

static const char *TAG = "lcd_diag";

/* ========================================================================
 * Common helper functions (shared by SPI and I80 diagnostics)
 * ======================================================================== */

static void diag_setup_gpio_out(int pin)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_set_level(pin, 1);
}

static void diag_setup_gpio_in(int pin)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
}

static void diag_setup_gpio_out_low(int pin)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_set_level(pin, 0);
}

/* ========================================================================
 * Common RDDID interpretation (shared by SPI and I80)
 * ======================================================================== */

void lcd_diag_print_rddi(uint32_t id)
{
    uint8_t b0 = (id >> 24) & 0xFF;
    uint8_t b1 = (id >> 16) & 0xFF;
    uint8_t b2 = (id >> 8) & 0xFF;
    uint8_t b3 = id & 0xFF;

    ESP_LOGI(TAG, "RDDID bytes: %02X %02X %02X %02X", b0, b1, b2, b3);

    uint16_t controller = ((uint16_t)b1 << 8) | b2;
    if (controller == 0x7796 || (b0 == 0x00 && b1 == 0x77 && b2 == 0x96)) {
        ESP_LOGI(TAG, "=> Controller identified: ST7796 / ST7796S");
    } else if (controller == 0x9488 || (b0 == 0x00 && b1 == 0x94 && b2 == 0x88)) {
        ESP_LOGI(TAG, "=> Controller identified: ILI9488");
    } else if (controller == 0x9341 || (b0 == 0x00 && b1 == 0x93 && b2 == 0x41)) {
        ESP_LOGI(TAG, "=> Controller identified: ILI9341");
    } else if (controller == 0x9325 || (b0 == 0x00 && b1 == 0x93 && b2 == 0x25)) {
        ESP_LOGI(TAG, "=> Controller identified: ILI9325");
    } else if (controller == 0x8357 || (b0 == 0x00 && b1 == 0x83 && b2 == 0x57)) {
        ESP_LOGI(TAG, "=> Controller identified: HX8357B/D");
    } else if (controller == 0x7789 || (b0 == 0x00 && b1 == 0x77 && b2 == 0x89)) {
        ESP_LOGI(TAG, "=> Controller identified: ST7789");
    } else if (id == 0x00000000 || id == 0xFFFFFFFF) {
        ESP_LOGW(TAG, "=> No valid RDDID response. Possible causes:");
        ESP_LOGW(TAG, "   - Wrong interface mode (IM pins)");
        ESP_LOGW(TAG, "   - Display not powered or wiring issue");
        ESP_LOGW(TAG, "   - Controller does not support RDDID readback");
    } else {
        ESP_LOGI(TAG, "=> Unknown controller ID: 0x%02X 0x%02X 0x%02X 0x%02X", b0, b1, b2, b3);
    }
}

/* ========================================================================
 * SPI bit-bang RDDID diagnostic
 * ======================================================================== */

#if CONFIG_IDMS_LCD_BUS_SPI || (!CONFIG_IDMS_LCD_BUS_I80 && !CONFIG_IDMS_LCD_BUS_SPI)

static void bb_delay(void)
{
    for (volatile int i = 0; i < 4; i++) {
    }
}

static void bb_send_bit(int mosi_pin, int sclk_pin, uint8_t bit)
{
    gpio_set_level(mosi_pin, bit);
    bb_delay();
    gpio_set_level(sclk_pin, 1);
    bb_delay();
    bb_delay();
    gpio_set_level(sclk_pin, 0);
    bb_delay();
}

static void bb_send_byte(int mosi_pin, int sclk_pin, uint8_t byte)
{
    for (int i = 7; i >= 0; i--) {
        bb_send_bit(mosi_pin, sclk_pin, (byte >> i) & 1);
    }
}

static uint8_t bb_read_byte(int miso_pin, int sclk_pin)
{
    uint8_t val = 0;
    for (int i = 7; i >= 0; i--) {
        gpio_set_level(sclk_pin, 1);
        bb_delay();
        bb_delay();
        val |= (gpio_get_level(miso_pin) & 1) << i;
        gpio_set_level(sclk_pin, 0);
        bb_delay();
    }
    return val;
}

static uint8_t bb_read_byte_mosi(int mosi_pin, int sclk_pin)
{
    uint8_t val = 0;
    gpio_set_direction(mosi_pin, GPIO_MODE_INPUT);
    bb_delay();
    for (int i = 7; i >= 0; i--) {
        gpio_set_level(sclk_pin, 1);
        bb_delay();
        bb_delay();
        val |= (gpio_get_level(mosi_pin) & 1) << i;
        gpio_set_level(sclk_pin, 0);
        bb_delay();
    }
    gpio_set_direction(mosi_pin, GPIO_MODE_OUTPUT);
    bb_delay();
    return val;
}

static void bb_send_cmd(int mosi_pin, int sclk_pin, int cs_pin, int dc_pin, uint8_t cmd)
{
    gpio_set_level(dc_pin, 0);
    bb_delay();
    gpio_set_level(cs_pin, 0);
    bb_delay();
    bb_send_byte(mosi_pin, sclk_pin, cmd);
    gpio_set_level(cs_pin, 1);
    bb_delay();
}

static void bb_send_cmd_then_read(int mosi_pin, int sclk_pin, int miso_pin,
                                  int cs_pin, int dc_pin,
                                  uint8_t cmd, uint8_t *rxbuf, int rxlen,
                                  bool use_miso)
{
    gpio_set_level(cs_pin, 1);
    bb_delay();
    gpio_set_level(dc_pin, 0);
    bb_delay();
    gpio_set_level(cs_pin, 0);
    bb_delay();
    bb_send_byte(mosi_pin, sclk_pin, cmd);

    vTaskDelay(pdMS_TO_TICKS(10));

    gpio_set_level(dc_pin, 1);
    bb_delay();

    for (int i = 0; i < rxlen; i++) {
        if (use_miso && miso_pin >= 0) {
            rxbuf[i] = bb_read_byte(miso_pin, sclk_pin);
        } else {
            rxbuf[i] = bb_read_byte_mosi(mosi_pin, sclk_pin);
        }
    }

    gpio_set_level(cs_pin, 1);
    bb_delay();
}

uint32_t lcd_diag_read_rddi(int sclk, int mosi, int miso, int cs, int dc, int rst)
{
    ESP_LOGI(TAG, "=== LCD Controller RDDID SPI Bit-Bang Diagnostic ===");
    ESP_LOGI(TAG, "Pins: SCLK=%d MOSI=%d MISO=%d CS=%d DC=%d RST=%d",
             sclk, mosi, miso, cs, dc, rst);

    diag_setup_gpio_out(sclk);
    diag_setup_gpio_out(mosi);
    if (miso >= 0) diag_setup_gpio_in(miso);
    diag_setup_gpio_out(cs);
    diag_setup_gpio_out(dc);

    gpio_set_level(cs, 1);
    gpio_set_level(dc, 1);
    gpio_set_level(sclk, 0);

    if (rst >= 0) {
        diag_setup_gpio_out(rst);
        ESP_LOGI(TAG, "Hardware reset via RST pin %d...", rst);
        gpio_set_level(rst, 0);
        vTaskDelay(pdMS_TO_TICKS(20));
        gpio_set_level(rst, 1);
        vTaskDelay(pdMS_TO_TICKS(150));
    } else {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ESP_LOGI(TAG, "Sending SW Reset (0x01) via bit-bang...");
    bb_send_cmd(mosi, sclk, cs, dc, 0x01);
    vTaskDelay(pdMS_TO_TICKS(200));

    ESP_LOGI(TAG, "Sending Sleep Out (0x11) via bit-bang...");
    bb_send_cmd(mosi, sclk, cs, dc, 0x11);
    vTaskDelay(pdMS_TO_TICKS(200));

    uint8_t rddid_miso[4] = {0};
    uint8_t rddid_mosi[4] = {0};
    uint8_t rdstatus[4] = {0};

    ESP_LOGI(TAG, "Reading RDDID (0x04) via MISO pin...");
    bb_send_cmd_then_read(mosi, sclk, miso, cs, dc,
                          0x04, rddid_miso, 4, true);

    ESP_LOGI(TAG, "  RDDID via MISO: %02X %02X %02X %02X",
             rddid_miso[0], rddid_miso[1], rddid_miso[2], rddid_miso[3]);

    ESP_LOGI(TAG, "Re-reading RDDID (0x04) via MOSI/SDA pin (bidir)...");
    bb_send_cmd_then_read(mosi, sclk, miso, cs, dc,
                          0x04, rddid_mosi, 4, false);

    ESP_LOGI(TAG, "  RDDID via SDA:  %02X %02X %02X %02X",
             rddid_mosi[0], rddid_mosi[1], rddid_mosi[2], rddid_mosi[3]);

    ESP_LOGI(TAG, "Reading RDDSDR (0x0F) via MISO pin...");
    bb_send_cmd_then_read(mosi, sclk, miso, cs, dc,
                          0x0F, rdstatus, 4, true);

    ESP_LOGI(TAG, "  RDDSDR via MISO: %02X %02X %02X %02X",
             rdstatus[0], rdstatus[1], rdstatus[2], rdstatus[3]);

    uint32_t id = 0;
    bool miso_valid = (rddid_miso[1] != 0 || rddid_miso[2] != 0 || rddid_miso[3] != 0);
    bool mosi_valid = (rddid_mosi[1] != 0 || rddid_mosi[2] != 0 || rddid_mosi[3] != 0);

    if (miso_valid) {
        id = ((uint32_t)rddid_miso[0] << 24) |
             ((uint32_t)rddid_miso[1] << 16) |
             ((uint32_t)rddid_miso[2] << 8) |
             ((uint32_t)rddid_miso[3]);
        ESP_LOGI(TAG, "Using MISO-based RDDID (valid data)");
    } else if (mosi_valid) {
        id = ((uint32_t)rddid_mosi[0] << 24) |
             ((uint32_t)rddid_mosi[1] << 16) |
             ((uint32_t)rddid_mosi[2] << 8) |
             ((uint32_t)rddid_mosi[3]);
        ESP_LOGI(TAG, "Using SDA-based RDDID (valid data)");
    } else {
        ESP_LOGW(TAG, "RDDID returned all zeros — display not responding or IM pins wrong");
    }

    gpio_set_level(cs, 1);
    gpio_set_level(dc, 1);

    ESP_LOGI(TAG, "=== SPI RDDID Diagnostic Complete ===");
    return id;
}

#endif /* CONFIG_IDMS_LCD_BUS_SPI || fallback */

/* ========================================================================
 * I80 parallel bus RDDID diagnostic
 * ======================================================================== */

#if CONFIG_IDMS_LCD_BUS_I80

static int s_i80_data_pins[8];

static void i80_delay(void)
{
    for (volatile int i = 0; i < 8; i++) {
    }
}

static void i80_setup_out(int pin)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_set_level(pin, 0);
}

static void i80_setup_in(int pin)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
}

static void i80_write_data(int wr_pin, int cs_pin, uint8_t val)
{
    for (int i = 0; i < 8; i++) {
        gpio_set_level(s_i80_data_pins[i], (val >> i) & 1);
    }
    gpio_set_level(cs_pin, 0);
    i80_delay();
    gpio_set_level(wr_pin, 0);
    i80_delay();
    gpio_set_level(wr_pin, 1);
    i80_delay();
    gpio_set_level(cs_pin, 1);
}

static uint8_t i80_read_data(int rd_pin, int cs_pin)
{
    for (int i = 0; i < 8; i++) {
        i80_setup_in(s_i80_data_pins[i]);
    }
    gpio_set_level(cs_pin, 0);
    i80_delay();
    gpio_set_level(rd_pin, 0);
    i80_delay();
    i80_delay();
    uint8_t val = 0;
    for (int i = 0; i < 8; i++) {
        val |= (gpio_get_level(s_i80_data_pins[i]) & 1) << i;
    }
    gpio_set_level(rd_pin, 1);
    i80_delay();
    gpio_set_level(cs_pin, 1);
    for (int i = 0; i < 8; i++) {
        i80_setup_out(s_i80_data_pins[i]);
    }
    return val;
}

static void i80_write_cmd(int dc_pin, int wr_pin, int cs_pin, uint8_t cmd)
{
    gpio_set_level(dc_pin, 0);
    i80_write_data(wr_pin, cs_pin, cmd);
}

static void i80_write_param(int dc_pin, int wr_pin, int cs_pin, uint8_t param)
{
    gpio_set_level(dc_pin, 1);
    i80_write_data(wr_pin, cs_pin, param);
}

uint32_t lcd_diag_read_rddi_i80(int d0, int d1, int d2, int d3, int d4, int d5, int d6, int d7,
                                 int wr, int rd, int cs, int dc, int rst)
{
    ESP_LOGI(TAG, "=== LCD Controller RDDID I80 Bit-Bang Diagnostic ===");
    ESP_LOGI(TAG, "Pins: D0=%d D1=%d D2=%d D3=%d D4=%d D5=%d D6=%d D7=%d",
             d0, d1, d2, d3, d4, d5, d6, d7);
    ESP_LOGI(TAG, "  WR=%d RD=%d CS=%d DC=%d RST=%d", wr, rd, cs, dc, rst);

    s_i80_data_pins[0] = d0;
    s_i80_data_pins[1] = d1;
    s_i80_data_pins[2] = d2;
    s_i80_data_pins[3] = d3;
    s_i80_data_pins[4] = d4;
    s_i80_data_pins[5] = d5;
    s_i80_data_pins[6] = d6;
    s_i80_data_pins[7] = d7;

    for (int i = 0; i < 8; i++) i80_setup_out(s_i80_data_pins[i]);
    diag_setup_gpio_out(cs);
    diag_setup_gpio_out(dc);
    diag_setup_gpio_out(wr);
    if (rd >= 0) diag_setup_gpio_out(rd);

    gpio_set_level(cs, 1);
    gpio_set_level(dc, 1);
    gpio_set_level(wr, 1);
    if (rd >= 0) gpio_set_level(rd, 1);

    if (rst >= 0) {
        diag_setup_gpio_out(rst);
        ESP_LOGI(TAG, "Hardware reset via RST pin %d...", rst);
        gpio_set_level(rst, 0);
        vTaskDelay(pdMS_TO_TICKS(20));
        gpio_set_level(rst, 1);
        vTaskDelay(pdMS_TO_TICKS(150));
    } else {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ESP_LOGI(TAG, "Sending SW Reset (0x01) via I80...");
    i80_write_cmd(dc, wr, cs, 0x01);
    vTaskDelay(pdMS_TO_TICKS(200));

    ESP_LOGI(TAG, "Sending Sleep Out (0x11) via I80...");
    i80_write_cmd(dc, wr, cs, 0x11);
    vTaskDelay(pdMS_TO_TICKS(200));

    uint8_t rddid[4] = {0};

    if (rd >= 0) {
        ESP_LOGI(TAG, "Reading RDDID (0x04) via I80 parallel bus...");
        gpio_set_level(dc, 0);
        i80_write_data(wr, cs, 0x04);
        vTaskDelay(pdMS_TO_TICKS(10));

        for (int i = 0; i < 4; i++) {
            rddid[i] = i80_read_data(rd, cs);
        }

        ESP_LOGI(TAG, "  RDDID: %02X %02X %02X %02X",
                 rddid[0], rddid[1], rddid[2], rddid[3]);
    } else {
        ESP_LOGW(TAG, "No RD pin — cannot read RDDID via I80 without RD strobe");
        ESP_LOGW(TAG, "Connect RD pin to a GPIO and set IDMS_PIN_LCD_I80_RD in menuconfig");

        i80_write_cmd(dc, wr, cs, 0x04);
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    gpio_set_level(cs, 1);
    gpio_set_level(dc, 1);
    gpio_set_level(wr, 1);
    if (rd >= 0) gpio_set_level(rd, 1);

    uint32_t id = 0;
    if (rd >= 0) {
        id = ((uint32_t)rddid[0] << 24) |
             ((uint32_t)rddid[1] << 16) |
             ((uint32_t)rddid[2] << 8) |
             ((uint32_t)rddid[3]);
    }

    ESP_LOGI(TAG, "=== I80 RDDID Diagnostic Complete ===");
    return id;
}

#endif /* CONFIG_IDMS_LCD_BUS_I80 */