#pragma once

#include <stdint.h>

/**
 * Print human-readable interpretation of the RDDID result to the log.
 * Works for both SPI and I80 read results.
 */
void lcd_diag_print_rddi(uint32_t id);

#if CONFIG_IDMS_LCD_BUS_SPI

/**
 * Read RDDID (0x04) from the LCD controller via GPIO bit-bang SPI.
 * MUST be called BEFORE spi_bus_initialize() since it takes over the SPI pins.
 */
uint32_t lcd_diag_read_rddi(int sclk, int mosi, int miso, int cs, int dc, int rst);

#elif CONFIG_IDMS_LCD_BUS_I80

/**
 * Read RDDID (0x04) from the LCD controller via I80 parallel bus.
 * MUST be called BEFORE esp_lcd_new_i80_bus() since it takes over the data pins.
 */
uint32_t lcd_diag_read_rddi_i80(int d0, int d1, int d2, int d3, int d4, int d5, int d6, int d7,
                                  int wr, int rd, int cs, int dc, int rst);

#endif