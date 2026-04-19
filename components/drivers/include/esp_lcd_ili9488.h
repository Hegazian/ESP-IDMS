#pragma once

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include <stdint.h>
#include <stdbool.h>

esp_err_t ili9488_lcd_init(esp_lcd_panel_io_handle_t io);
esp_err_t ili9488_lcd_mirror(esp_lcd_panel_io_handle_t io, bool mirror_x, bool mirror_y);
esp_err_t ili9488_lcd_swap_xy(esp_lcd_panel_io_handle_t io, bool swap);
esp_err_t ili9488_lcd_invert_color(esp_lcd_panel_io_handle_t io, bool invert);
esp_err_t ili9488_lcd_fill_color(esp_lcd_panel_io_handle_t io, uint16_t color, int width, int height);