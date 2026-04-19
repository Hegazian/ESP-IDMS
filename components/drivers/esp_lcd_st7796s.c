#include "esp_lcd_st7796s.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdint.h>

static const char *TAG = "st7796s";

static esp_err_t send_cmd(esp_lcd_panel_io_handle_t io, uint8_t cmd)
{
    return esp_lcd_panel_io_tx_param(io, cmd, NULL, 0);
}

static esp_err_t send_data(esp_lcd_panel_io_handle_t io, uint8_t cmd, const uint8_t *data, uint16_t len)
{
    return esp_lcd_panel_io_tx_param(io, cmd, data, len);
}

esp_err_t st7796s_lcd_init(esp_lcd_panel_io_handle_t io)
{
    ESP_LOGI(TAG, "Initializing ST7796 LCD (320x480)");

    /* Software Reset */
    send_cmd(io, 0x01);
    vTaskDelay(pdMS_TO_TICKS(200));

    /* Sleep Out */
    send_cmd(io, 0x11);
    vTaskDelay(pdMS_TO_TICKS(200));

    /* Memory Data Access Control: BGR */
    uint8_t madctl = 0x00;
    madctl |= (1 << 3); /* BGR */
    send_data(io, 0x36, &madctl, 1);

    /* Interface Pixel Format: 16-bit RGB565 */
    uint8_t pixfmt = 0x55;
    send_data(io, 0x3A, &pixfmt, 1);

    /* Porch Setting */
    uint8_t porch[5] = {0x0C, 0x0C, 0x00, 0x33, 0x33};
    send_data(io, 0xB2, porch, 5);

    /* Gate Control */
    uint8_t gate = 0x35;
    send_data(io, 0xB7, &gate, 1);

    /* VCOM Setting */
    uint8_t vcom = 0x19;
    send_data(io, 0xBB, &vcom, 1);

    /* LCM Control */
    send_data(io, 0xC0, (uint8_t[]){0x2C, 0x2C}, 2);

    /* VCOM Voltage Setting */
    send_data(io, 0xC2, (uint8_t[]){0x01, 0xFF}, 2);

    /* VDV Setting */
    uint8_t vdv = 0x20;
    send_data(io, 0xC5, &vdv, 1);

    /* Frame Rate Control: 60Hz */
    uint8_t frate = 0x0F;
    send_data(io, 0xC6, &frate, 1);

    /* Power Control 1 */
    send_data(io, 0xD0, (uint8_t[]){0xA4, 0xA1}, 2);

    /* Positive Gamma */
    send_data(io, 0xE0,
        (uint8_t[]){0xD0, 0x08, 0x0E, 0x09, 0x09, 0x05, 0x31, 0x33,
                     0x48, 0x17, 0x14, 0x15, 0x31, 0x34}, 14);

    /* Negative Gamma */
    send_data(io, 0xE1,
        (uint8_t[]){0xD0, 0x08, 0x0E, 0x09, 0x09, 0x15, 0x31, 0x33,
                     0x48, 0x17, 0x14, 0x15, 0x31, 0x34}, 14);

    /* Column Address Set: 0..319 */
    send_data(io, 0x2A, (uint8_t[]){0x00, 0x00, 0x01, 0x3F}, 4);

    /* Row Address Set: 0..479 */
    send_data(io, 0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xDF}, 4);

    /* NOTE: Inversion OFF for now - try both IPS and TFT modes */
    /* Do NOT enable inversion (0x21) — white screen may be caused by wrong inversion */
    /* If display shows colors inverted, we'll enable it later */

    /* Display On */
    send_cmd(io, 0x29);
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "ST7796 initialized (MADCTL=0x%02X, inversion=OFF)", madctl);
    return ESP_OK;
}

esp_err_t st7796s_lcd_mirror(esp_lcd_panel_io_handle_t io, bool mirror_x, bool mirror_y)
{
    uint8_t madctl = 0x00;
    madctl |= (1 << 3); /* BGR always on */
    if (mirror_x) madctl |= (1 << 6); /* MX: column address order */
    if (mirror_y) madctl |= (1 << 7); /* MY: row address order */
    return send_data(io, 0x36, &madctl, 1);
}

esp_err_t st7796s_lcd_swap_xy(esp_lcd_panel_io_handle_t io, bool swap)
{
    uint8_t madctl = 0x00;
    madctl |= (1 << 3); /* BGR */
    if (swap) madctl |= (1 << 5); /* MV: row/column exchange */
    return send_data(io, 0x36, &madctl, 1);
}

esp_err_t st7796s_lcd_invert_color(esp_lcd_panel_io_handle_t io, bool invert)
{
    return send_cmd(io, invert ? 0x21 : 0x20);
}

esp_err_t st7796s_lcd_fill_color(esp_lcd_panel_io_handle_t io, uint16_t color, int width, int height)
{
    ESP_LOGI(TAG, "Fill screen %dx%d with color 0x%04X", width, height, color);

    uint8_t col[4] = {0x00, 0x00, (uint8_t)((width - 1) >> 8), (uint8_t)((width - 1) & 0xFF)};
    uint8_t row[4] = {0x00, 0x00, (uint8_t)((height - 1) >> 8), (uint8_t)((height - 1) & 0xFF)};
    esp_err_t err;

    err = esp_lcd_panel_io_tx_param(io, 0x2A, col, 4);
    if (err != ESP_OK) { ESP_LOGE(TAG, "CASET failed: %s", esp_err_to_name(err)); return err; }

    err = esp_lcd_panel_io_tx_param(io, 0x2B, row, 4);
    if (err != ESP_OK) { ESP_LOGE(TAG, "RASET failed: %s", esp_err_to_name(err)); return err; }

    int chunk_lines = 10;
    size_t chunk_px = width * chunk_lines;
    size_t chunk_bytes = chunk_px * sizeof(uint16_t);

    uint16_t *buf = heap_caps_malloc(chunk_bytes, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!buf) {
        buf = heap_caps_malloc(chunk_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!buf) {
        ESP_LOGE(TAG, "Failed to allocate fill buffer (%zu bytes)", chunk_bytes);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Fill buffer %zu bytes allocated at %p", chunk_bytes, (void *)buf);

    for (int i = 0; i < chunk_px; i++) {
        buf[i] = color;
    }

    for (int y = 0; y < height; y += chunk_lines) {
        int lines = chunk_lines;
        if (y + lines > height) lines = height - y;

        uint8_t wr_col[4] = {0x00, 0x00, (uint8_t)((width - 1) >> 8), (uint8_t)((width - 1) & 0xFF)};
        uint8_t wr_row[4] = {
            (uint8_t)(y >> 8), (uint8_t)(y & 0xFF),
            (uint8_t)((y + lines - 1) >> 8), (uint8_t)((y + lines - 1) & 0xFF)
        };
        err = esp_lcd_panel_io_tx_param(io, 0x2A, wr_col, 4);
        if (err != ESP_OK) { ESP_LOGE(TAG, "CASET row failed: %s", esp_err_to_name(err)); break; }
        err = esp_lcd_panel_io_tx_param(io, 0x2B, wr_row, 4);
        if (err != ESP_OK) { ESP_LOGE(TAG, "RASET row failed: %s", esp_err_to_name(err)); break; }

        size_t len = width * lines * sizeof(uint16_t);
        err = esp_lcd_panel_io_tx_color(io, 0x2C, buf, len);
        if (err != ESP_OK) { ESP_LOGE(TAG, "Color tx failed at y=%d: %s", y, esp_err_to_name(err)); break; }

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    free(buf);
    ESP_LOGI(TAG, "Fill complete");
    return ESP_OK;
}

esp_err_t st7796s_lcd_test_read_id(esp_lcd_panel_io_handle_t io)
{
    ESP_LOGI(TAG, "=== LCD SPI DIAGNOSTIC ===");
    ESP_LOGI(TAG, "Expected IM[2:0]=111 for 4-wire SPI. Check your display IM pins!");
    ESP_LOGI(TAG, "If IM pins are wrong, display stays white (commands accepted but no pixel output).");

    /* Verify GPIO states at boot */
    ESP_LOGI(TAG, "GPIO outputs: DC=%d, CS=%d, SCLK=18, MOSI=9, RST=12",
             CONFIG_IDMS_PIN_LCD_DC, CONFIG_IDMS_PIN_LCD_CS);

    /* Toggle DC pin to verify it's working */
    int dc_pin = CONFIG_IDMS_PIN_LCD_DC;
    gpio_set_level(dc_pin, 1);
    vTaskDelay(pdMS_TO_TICKS(1));
    ESP_LOGI(TAG, "DC pin set HIGH, read back: %d", gpio_get_level(dc_pin));
    gpio_set_level(dc_pin, 0);
    vTaskDelay(pdMS_TO_TICKS(1));
    ESP_LOGI(TAG, "DC pin set LOW, read back: %d", gpio_get_level(dc_pin));

    /* Send RDDID command (0x04) and verify SPI bus responds */
    esp_err_t err = esp_lcd_panel_io_tx_param(io, 0x04, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RDDID command failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "RDDID command sent OK (SPI bus responsive)");

    /* Send MADCTL read attempt */
    uint8_t madctl_val = 0x28;
    err = esp_lcd_panel_io_tx_param(io, 0x36, &madctl_val, 1);
    ESP_LOGI(TAG, "MADCTL write (0x28) result: %s", esp_err_to_name(err));

    /* Toggle backlight to verify display power */
    if (CONFIG_IDMS_PIN_LCD_BL >= 0) {
        ESP_LOGI(TAG, "Toggling backlight on pin %d...", CONFIG_IDMS_PIN_LCD_BL);
        for (int i = 0; i < 3; i++) {
            gpio_set_level(CONFIG_IDMS_PIN_LCD_BL, 0);
            vTaskDelay(pdMS_TO_TICKS(500));
            gpio_set_level(CONFIG_IDMS_PIN_LCD_BL, 1);
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        ESP_LOGI(TAG, "Did the backlight blink? If not, check BL wiring!");
    }

    ESP_LOGI(TAG, "=== END LCD DIAGNOSTIC ===");
    return ESP_OK;
}