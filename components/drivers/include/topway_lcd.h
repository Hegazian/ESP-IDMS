/**
 * @file topway_lcd.h
 * @brief Topway Smart LCD driver - VP Memory Protocol
 *
 * Topway Smart LCDs use VP (Variable Pointer) memory for data exchange.
 * Host MCU writes to specific memory addresses to update display values.
 */

#ifndef TOPWAY_LCD_H
#define TOPWAY_LCD_H

#include "driver/uart.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Topway frame header */
#define TOPWAY_FRAME_HEAD 0x5A
#define TOPWAY_FRAME_TAIL 0xA5

/* VP Memory Write Commands */
#define TOPWAY_CMD_WRITE_8BIT 0x82  /* Write 8-bit/16-bit variable */
#define TOPWAY_CMD_WRITE_32BIT 0x83 /* Write 32-bit variable */
#define TOPWAY_CMD_READ 0x81        /* Read variable */
#define TOPWAY_CMD_UPLOAD_PIC 0x88  /* Upload picture */

/* System Registers (VP addresses) */
#define TOPWAY_REG_BACKLIGHT 0xFFFF21 /* Backlight brightness (0-100) */
#define TOPWAY_REG_BUZZER 0xFFFF20    /* Buzzer control */
#define TOPWAY_REG_PAGE 0xFFFF00      /* Current page number */
#define TOPWAY_REG_RTC_YEAR 0xFFFF10
#define TOPWAY_REG_RTC_MONTH 0xFFFF11
#define TOPWAY_REG_RTC_DAY 0xFFFF12
#define TOPWAY_REG_RTC_HOUR 0xFFFF13
#define TOPWAY_REG_RTC_MIN 0xFFFF14
#define TOPWAY_REG_RTC_SEC 0xFFFF15

/* Color definitions (RGB565) */
#define TOPWAY_COLOR_BLACK 0x0000
#define TOPWAY_COLOR_WHITE 0xFFFF
#define TOPWAY_COLOR_RED 0xF800
#define TOPWAY_COLOR_GREEN 0x07E0
#define TOPWAY_COLOR_BLUE 0x001F
#define TOPWAY_COLOR_YELLOW 0xFFE0
#define TOPWAY_COLOR_CYAN 0x07FF
#define TOPWAY_COLOR_MAGENTA 0xF81F
#define TOPWAY_COLOR_ORANGE 0xFD20
#define TOPWAY_COLOR_GRAY 0x8410

/* ESP-IDMS VP Variable Addresses (user configurable) */
#define VP_CURRENT_VALUE 0x081000 /* Machine current (float) */
#define VP_CURRENT_VALID 0x081004 /* Current valid flag */
#define VP_TIN_VALUE 0x031008     /* Temperature in (float) */
#define VP_TIN_VALID 0x03100C     /* T_in valid flag */
#define VP_TOUT_VALUE 0x031010    /* Temperature out (float) */
#define VP_TOUT_VALID 0x031014    /* T_out valid flag */
#define VP_DELTA_T_VALUE 0x031018 /* Delta T (float) */
#define VP_DELTA_T_VALID 0x03101C /* Delta T valid flag */
#define VP_WIFI_STATUS 0x1020     /* WiFi connected (0/1) */
#define VP_OTA_STATUS 0x1024      /* OTA status code */
#define VP_TECH_COUNT 0x1028      /* Technician count */
#define VP_POWER_FAULT 0x102C     /* Power fault flag */
#define VP_COOL_FAULT 0x1030      /* Cooling fault flag */
#define VP_TEXT_BUFFER 0x2000     /* String variable start */

/* WiFi status codes */
#define WIFI_OFFLINE 0
#define WIFI_CONNECTED 1

/* OTA status codes */
#define OTA_READY 0
#define OTA_UPDATING 1
#define OTA_ERROR 2

/**
 * @brief Topway display configuration
 */
typedef struct {
  uart_port_t uart_port; /* UART port number */
  int tx_pin;            /* TX GPIO pin */
  int rx_pin;            /* RX GPIO pin */
  int baud_rate;         /* Baud rate (typically 115200) */
  uint16_t width;        /* Display width (800) */
  uint16_t height;       /* Display height (480) */
} topway_config_t;

/**
 * @brief Initialize Topway display
 */
esp_err_t topway_init(const topway_config_t *config);

/**
 * @brief Deinitialize Topway display
 */
esp_err_t topway_deinit(void);

/**
 * @brief Set active page
 */
esp_err_t topway_set_page(uint8_t page);

/**
 * @brief Write 16-bit variable to VP memory
 */
esp_err_t topway_write_vp16(uint32_t addr, uint16_t value);

/**
 * @brief Write 32-bit variable to VP memory (for floats)
 */
esp_err_t topway_write_vp32(uint32_t addr, uint32_t value);

/**
 * @brief Write float to VP memory
 */
esp_err_t topway_write_float(uint32_t addr, float value);

/**
 * @brief Write string to VP memory
 */
esp_err_t topway_write_string(uint32_t addr, const char *str);

/**
 * @brief Set backlight brightness
 */
esp_err_t topway_set_brightness(uint8_t brightness);

/**
 * @brief Clear screen with color
 */
esp_err_t topway_clear_screen(uint16_t color);

/**
 * @brief Draw filled rectangle
 */
esp_err_t topway_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                           uint16_t color);

/**
 * @brief Draw line
 */
esp_err_t topway_draw_line(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2,
                           uint16_t color);

/**
 * @brief Draw text at position
 */
esp_err_t topway_draw_text(uint16_t x, uint16_t y, const char *text,
                           uint16_t color, uint8_t font_size);

#ifdef __cplusplus
}
#endif

#endif /* TOPWAY_LCD_H */
