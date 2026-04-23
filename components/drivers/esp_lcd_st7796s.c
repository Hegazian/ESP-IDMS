#include "esp_lcd_st7796s.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdint.h>

static const char *TAG = "st7796s";

#define ST7796S_NATIVE_W 320
#define ST7796S_NATIVE_H 480

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
    ESP_LOGI(TAG, "Initializing ST7796S LCD (TFT_eSPI proven sequence)");

    send_cmd(io, 0x01);
    vTaskDelay(pdMS_TO_TICKS(120));

    send_cmd(io, 0x11);
    vTaskDelay(pdMS_TO_TICKS(120));

    send_data(io, 0xF0, (uint8_t[]){0xC3}, 1);
    send_data(io, 0xF0, (uint8_t[]){0x96}, 1);

    send_data(io, 0x36, (uint8_t[]){0x48}, 1);
    send_data(io, 0x3A, (uint8_t[]){0x55}, 1);
    send_data(io, 0xB4, (uint8_t[]){0x01}, 1);
    send_data(io, 0xB6, (uint8_t[]){0x80, 0x02, 0x3B}, 3);
    send_data(io, 0xE8, (uint8_t[]){0x40, 0x8A, 0x00, 0x00, 0x29, 0x19, 0xA5, 0x33}, 8);
    send_data(io, 0xC1, (uint8_t[]){0x06}, 1);
    send_data(io, 0xC2, (uint8_t[]){0xA7}, 1);
    send_data(io, 0xC5, (uint8_t[]){0x18}, 1);

    vTaskDelay(pdMS_TO_TICKS(120));

    send_data(io, 0xE0,
        (uint8_t[]){0xF0, 0x09, 0x0B, 0x06, 0x04, 0x15, 0x2F, 0x54,
                     0x42, 0x3C, 0x17, 0x14, 0x18, 0x1B}, 14);
    send_data(io, 0xE1,
        (uint8_t[]){0xE0, 0x09, 0x0B, 0x06, 0x04, 0x03, 0x2B, 0x43,
                     0x42, 0x3B, 0x16, 0x14, 0x17, 0x1B}, 14);

    vTaskDelay(pdMS_TO_TICKS(120));

    send_data(io, 0xF0, (uint8_t[]){0x3C}, 1);
    send_data(io, 0xF0, (uint8_t[]){0x69}, 1);

    vTaskDelay(pdMS_TO_TICKS(120));

    uint8_t madctl = 0x28;
    send_data(io, 0x36, &madctl, 1);
    send_data(io, 0x2A, (uint8_t[]){0x00, 0x00, 0x01, 0xDF}, 4);
    send_data(io, 0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0x3F}, 4);

    send_cmd(io, 0x29);
    vTaskDelay(pdMS_TO_TICKS(120));

    ESP_LOGI(TAG, "ST7796S initialized (MADCTL=0x%02X, no inversion)", madctl);
    return ESP_OK;
}

esp_err_t st7796s_lcd_mirror(esp_lcd_panel_io_handle_t io, bool mirror_x, bool mirror_y)
{
    uint8_t madctl = 0x08;
    madctl |= (1 << 5);
    if (mirror_x) madctl |= (1 << 6);
    if (mirror_y) madctl |= (1 << 7);
    return send_data(io, 0x36, &madctl, 1);
}

esp_err_t st7796s_lcd_swap_xy(esp_lcd_panel_io_handle_t io, bool swap)
{
    uint8_t madctl = 0x08;
    if (swap) madctl |= (1 << 5);
    return send_data(io, 0x36, &madctl, 1);
}

esp_err_t st7796s_lcd_invert_color(esp_lcd_panel_io_handle_t io, bool invert)
{
    return send_cmd(io, invert ? 0x21 : 0x20);
}

esp_err_t st7796s_lcd_fill_color(esp_lcd_panel_io_handle_t io, uint16_t color, int width, int height)
{
    ESP_LOGI(TAG, "Fill screen %dx%d with color 0x%04X", width, height, color);

    /* With MV=1 landscape: Column address = native Y range, Row address = native X range
     * We fill the entire screen: CAS 0..479, RAS 0..319 */
    uint8_t col[4] = {0x00, 0x00, (uint8_t)((ST7796S_NATIVE_H - 1) >> 8), (uint8_t)((ST7796S_NATIVE_H - 1) & 0xFF)};
    uint8_t row[4] = {0x00, 0x00, (uint8_t)((ST7796S_NATIVE_W - 1) >> 8), (uint8_t)((ST7796S_NATIVE_W - 1) & 0xFF)};

    esp_err_t err;
    err = esp_lcd_panel_io_tx_param(io, 0x2A, col, 4);
    if (err != ESP_OK) { ESP_LOGE(TAG, "CASET failed: %s", esp_err_to_name(err)); return err; }
    err = esp_lcd_panel_io_tx_param(io, 0x2B, row, 4);
    if (err != ESP_OK) { ESP_LOGE(TAG, "RASET failed: %s", esp_err_to_name(err)); return err; }

    int total_px = ST7796S_NATIVE_W * ST7796S_NATIVE_H;
    int chunk_px = ST7796S_NATIVE_W * 10;
    size_t chunk_bytes = chunk_px * sizeof(uint16_t);

    uint16_t *buf = heap_caps_malloc(chunk_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!buf) {
        buf = heap_caps_malloc(chunk_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!buf) {
        ESP_LOGE(TAG, "Failed to allocate fill buffer (%zu bytes)", chunk_bytes);
        return ESP_ERR_NO_MEM;
    }

    for (int i = 0; i < chunk_px; i++) {
        buf[i] = color;
    }

    for (int offset = 0; offset < total_px; offset += chunk_px) {
        int remaining = total_px - offset;
        int this_px = (remaining < chunk_px) ? remaining : chunk_px;
        size_t len = this_px * sizeof(uint16_t);
        err = esp_lcd_panel_io_tx_color(io, 0x2C, buf, len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Color tx failed at px %d: %s", offset, esp_err_to_name(err));
            break;
        }
    }

    free(buf);
    ESP_LOGI(TAG, "Fill complete");
    return ESP_OK;
}

esp_err_t st7796s_lcd_read_id(esp_lcd_panel_io_handle_t io, uint8_t *out_id, size_t out_sz)
{
    if (!out_id || out_sz < 4) return ESP_ERR_INVALID_ARG;

    memset(out_id, 0, out_sz);
    uint8_t dummy = 0;
    esp_err_t err = esp_lcd_panel_io_tx_param(io, 0x04, &dummy, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RDDID command send failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_lcd_panel_io_rx_param(io, 0x04, out_id, 4);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RDDID read failed: %s (display may not support readback over SPI)", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "RDDID: %02X %02X %02X %02X", out_id[0], out_id[1], out_id[2], out_id[3]);
    uint16_t controller = ((uint16_t)out_id[1] << 8) | out_id[2];
    if (controller == 0x7796) {
        ESP_LOGI(TAG, "Controller confirmed: ST7796S");
    } else if (controller == 0x0000 && out_id[0] == 0x00 && out_id[3] == 0x00) {
        ESP_LOGW(TAG, "RDDID all zeros — display not responding");
        ESP_LOGW(TAG, "  Possible causes:");
        ESP_LOGW(TAG, "  1. IM pins wrong (need IM=111 for SPI 4-wire)");
        ESP_LOGW(TAG, "  2. Display not powered or wiring issue");
        ESP_LOGW(TAG, "  3. No MISO wire connected (RDDID requires MISO)");
    } else {
        ESP_LOGW(TAG, "Unexpected controller ID: 0x%04X (expected 0x7796)", controller);
    }

    return ESP_OK;
}

esp_err_t st7796s_lcd_selftest(esp_lcd_panel_io_handle_t io)
{
    ESP_LOGI(TAG, "=== ST7796S Self-Test ===");

    uint8_t id[4] = {0};
    esp_err_t err = st7796s_lcd_read_id(io, id, sizeof(id));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "RDDID read failed — SPI readback may not be wired");
    }

    if (CONFIG_IDMS_PIN_LCD_BL >= 0) {
        ESP_LOGI(TAG, "Toggling backlight on GPIO %d...", CONFIG_IDMS_PIN_LCD_BL);
        for (int i = 0; i < 3; i++) {
            gpio_set_level(CONFIG_IDMS_PIN_LCD_BL, 0);
            vTaskDelay(pdMS_TO_TICKS(300));
            gpio_set_level(CONFIG_IDMS_PIN_LCD_BL, 1);
            vTaskDelay(pdMS_TO_TICKS(300));
        }
        ESP_LOGI(TAG, "Did the backlight blink? If not, check BL wiring!");
    }

    ESP_LOGI(TAG, "Filling screen RED...");
    err = st7796s_lcd_fill_color(io, 0xF800, 480, 320);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Red fill failed: %s", esp_err_to_name(err));
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(1000));

    ESP_LOGI(TAG, "Filling screen GREEN...");
    st7796s_lcd_fill_color(io, 0x07E0, 480, 320);
    vTaskDelay(pdMS_TO_TICKS(1000));

    ESP_LOGI(TAG, "Filling screen BLUE...");
    st7796s_lcd_fill_color(io, 0x001F, 480, 320);
    vTaskDelay(pdMS_TO_TICKS(1000));

    ESP_LOGI(TAG, "Clearing screen (black)...");
    st7796s_lcd_fill_color(io, 0x0000, 480, 320);

    ESP_LOGI(TAG, "=== Self-Test Complete ===");
    ESP_LOGI(TAG, "If you saw RED/GREEN/BLUE, display is working!");
    ESP_LOGI(TAG, "If screen stayed WHITE, check IM pins and wiring");
    return ESP_OK;
}
