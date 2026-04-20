#include "esp_lcd_ili9488.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdint.h>

static const char *TAG = "ili9488";

static esp_err_t send_cmd(esp_lcd_panel_io_handle_t io, uint8_t cmd)
{
    return esp_lcd_panel_io_tx_param(io, cmd, NULL, 0);
}

static esp_err_t send_data(esp_lcd_panel_io_handle_t io, uint8_t cmd, const uint8_t *data, uint16_t len)
{
    return esp_lcd_panel_io_tx_param(io, cmd, data, len);
}

esp_err_t ili9488_lcd_init(esp_lcd_panel_io_handle_t io)
{
    ESP_LOGI(TAG, "Initializing ILI9488 LCD (480x320 landscape)");

    /* Software Reset */
    send_cmd(io, 0x01);
    vTaskDelay(pdMS_TO_TICKS(200));

    /* Sleep Out */
    send_cmd(io, 0x11);
    vTaskDelay(pdMS_TO_TICKS(200));

    /* Interface Pixel Format: 16-bit RGB565 (0x55) */
    uint8_t pixfmt = 0x55;
    send_data(io, 0x3A, &pixfmt, 1);

    /* Memory Data Access Control: BGR + MV (landscape rotation) */
    uint8_t madctl = 0x00;
    madctl |= (1 << 3); /* BGR */
    madctl |= (1 << 5); /* MV: row/column exchange for landscape */
    send_data(io, 0x36, &madctl, 1);

    /* Porch Setting - Normal mode */
    send_data(io, 0xF2, (uint8_t[]){0x00}, 1);

    /* Frame Rate Control: 60Hz */
    send_data(io, 0xB4, (uint8_t[]){0x00, 0x00, 0x00}, 3);

    /* Power Control 1 */
    send_data(io, 0xC0, (uint8_t[]){0x0D, 0x0D}, 2);

    /* Power Control 2 */
    send_data(io, 0xC1, (uint8_t[]){0x41, 0x00}, 2);

    /* Power Control 3 */
    send_data(io, 0xC5, (uint8_t[]){0x00, 0x22}, 2);

    /* VCOM Control */
    send_data(io, 0xC5, (uint8_t[]){0x00, 0x22}, 2);

    /* Positive Gamma Correction */
    send_data(io, 0xE0,
        (uint8_t[]){0x00, 0x0C, 0x11, 0x04, 0x11, 0x08, 0x37, 0x89,
                     0x4C, 0x06, 0x0E, 0x01, 0x00, 0x00, 0x00, 0x00}, 16);

    /* Negative Gamma Correction */
    send_data(io, 0xE1,
        (uint8_t[]){0x00, 0x28, 0x25, 0x01, 0x0E, 0x04, 0x48, 0x84,
                     0x42, 0x04, 0x0C, 0x02, 0x00, 0x00, 0x00, 0x00}, 16);

    /* Column Address Set: 0..479 (landscape width) */
    send_data(io, 0x2A, (uint8_t[]){0x00, 0x00, 0x01, 0xDF}, 4);

    /* Row Address Set: 0..319 (landscape height) */
    send_data(io, 0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0x3F}, 4);

    /* Enable Color Inversion - CRITICAL: Most TFT panels need this ON */
    send_cmd(io, 0x21);
    vTaskDelay(pdMS_TO_TICKS(10));

    /* Display On */
    send_cmd(io, 0x29);
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "ILI9488 initialized (MADCTL=0x%02X, inversion=ON)", madctl);
    return ESP_OK;
}

esp_err_t ili9488_lcd_mirror(esp_lcd_panel_io_handle_t io, bool mirror_x, bool mirror_y)
{
    uint8_t madctl = 0x00;
    madctl |= (1 << 3); /* BGR */
    if (mirror_x) madctl |= (1 << 6); /* MX */
    if (mirror_y) madctl |= (1 << 7); /* MY */
    return send_data(io, 0x36, &madctl, 1);
}

esp_err_t ili9488_lcd_swap_xy(esp_lcd_panel_io_handle_t io, bool swap)
{
    uint8_t madctl = 0x00;
    madctl |= (1 << 3); /* BGR */
    if (swap) madctl |= (1 << 5); /* MV */
    return send_data(io, 0x36, &madctl, 1);
}

esp_err_t ili9488_lcd_invert_color(esp_lcd_panel_io_handle_t io, bool invert)
{
    return send_cmd(io, invert ? 0x21 : 0x20);
}

esp_err_t ili9488_lcd_fill_color(esp_lcd_panel_io_handle_t io, uint16_t color, int width, int height)
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