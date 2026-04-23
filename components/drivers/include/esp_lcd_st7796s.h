#pragma once

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include <stdbool.h>

/**
 * Send ST7796S initialization sequence to the display.
 * Must be called after the SPI panel IO is created and before any drawing.
 *
 * @param io  Panel IO handle (from esp_lcd_new_panel_io_spi)
 * @return    ESP_OK on success
 */
esp_err_t st7796s_lcd_init(esp_lcd_panel_io_handle_t io);

/**
 * Send ST7796S commands to set mirror/swap/invert via the panel IO.
 * These modify MADCTL register directly.
 */
esp_err_t st7796s_lcd_mirror(esp_lcd_panel_io_handle_t io, bool mirror_x, bool mirror_y);
esp_err_t st7796s_lcd_swap_xy(esp_lcd_panel_io_handle_t io, bool swap);
esp_err_t st7796s_lcd_invert_color(esp_lcd_panel_io_handle_t io, bool invert);

/**
 * Fill the entire screen with a solid 16-bit RGB565 color.
 * Useful for testing display connectivity before LVGL starts.
 */
esp_err_t st7796s_lcd_fill_color(esp_lcd_panel_io_handle_t io, uint16_t color, int width, int height);

/**
 * Read the display ID (command 0x04) to verify SPI communication.
 * Expected: 0x00 0x77 0x96 0x00 for ST7796S.
 */
esp_err_t st7796s_lcd_read_id(esp_lcd_panel_io_handle_t io, uint8_t *out_id, size_t out_sz);

/**
 * Validate that the ST7796S is actually listening on SPI (IM pins = 111).
 * Reads RDDID and checks for expected controller ID.
 * Logs detailed diagnostics if the display does not respond.
 */
esp_err_t st7796s_lcd_validate_im_pins(esp_lcd_panel_io_handle_t io);