/**
 * @file topway_lcd.c
 * @brief Topway Smart LCD driver - VP Memory Protocol Implementation
 */

#include "topway_lcd.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

static const char *TAG = "topway";

static uart_port_t s_uart_port = UART_NUM_1;

/**
 * @brief Send raw data to Topway display
 */
static esp_err_t send_raw(const uint8_t *data, size_t len)
{
    return uart_write_bytes(s_uart_port, data, len);
}

/**
 * @brief Calculate float bits for sending
 */
static uint32_t float_to_bits(float f)
{
    union { float f; uint32_t u; } converter;
    converter.f = f;
    return converter.u;
}

esp_err_t topway_init(const topway_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;

    s_uart_port = config->uart_port;

    uart_config_t uart_config = {
        .baud_rate = config->baud_rate ? config->baud_rate : 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };

    ESP_ERROR_CHECK(uart_param_config(s_uart_port, &uart_config));
    ESP_ERROR_CHECK(uart_driver_install(s_uart_port, 1024, 1024, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_set_pin(s_uart_port, config->tx_pin, config->rx_pin, 
                                  UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "Topway LCD: %dx%d, UART%d @ %d baud (TX=%d, RX=%d)",
             config->width, config->height, s_uart_port, 
             config->baud_rate, config->tx_pin, config->rx_pin);

    return ESP_OK;
}

esp_err_t topway_deinit(void)
{
    return uart_driver_delete(s_uart_port);
}

esp_err_t topway_write_vp16(uint32_t addr, uint16_t value)
{
    /* Write 16-bit variable: 5A A5 82 [addr_h] [addr_l] [val_h] [val_l] */
    uint8_t cmd[7] = {
        TOPWAY_FRAME_HEAD,
        TOPWAY_FRAME_TAIL,
        TOPWAY_CMD_WRITE_8BIT,
        (addr >> 8) & 0xFF,
        addr & 0xFF,
        (value >> 8) & 0xFF,
        value & 0xFF
    };
    return send_raw(cmd, sizeof(cmd));
}

esp_err_t topway_write_vp32(uint32_t addr, uint32_t value)
{
    /* Write 32-bit variable: 5A A5 83 [addr_h] [addr_l] [val3] [val2] [val1] [val0] */
    uint8_t cmd[9] = {
        TOPWAY_FRAME_HEAD,
        TOPWAY_FRAME_TAIL,
        TOPWAY_CMD_WRITE_32BIT,
        (addr >> 8) & 0xFF,
        addr & 0xFF,
        (value >> 24) & 0xFF,
        (value >> 16) & 0xFF,
        (value >> 8) & 0xFF,
        value & 0xFF
    };
    return send_raw(cmd, sizeof(cmd));
}

esp_err_t topway_write_float(uint32_t addr, float value)
{
    return topway_write_vp32(addr, float_to_bits(value));
}

esp_err_t topway_write_string(uint32_t addr, const char *str)
{
    if (!str) return ESP_ERR_INVALID_ARG;
    
    size_t len = strlen(str);
    if (len > 248) len = 248;
    
    /* String write: 5A A5 82 [addr_h] [addr_l] [len] [string...] */
    uint8_t cmd[256];
    cmd[0] = TOPWAY_FRAME_HEAD;
    cmd[1] = TOPWAY_FRAME_TAIL;
    cmd[2] = TOPWAY_CMD_WRITE_8BIT;
    cmd[3] = (addr >> 8) & 0xFF;
    cmd[4] = addr & 0xFF;
    cmd[5] = len;
    memcpy(&cmd[6], str, len);
    
    return send_raw(cmd, len + 6);
}

esp_err_t topway_set_page(uint8_t page)
{
    return topway_write_vp16(TOPWAY_REG_PAGE, page);
}

esp_err_t topway_set_brightness(uint8_t brightness)
{
    if (brightness > 100) brightness = 100;
    return topway_write_vp16(TOPWAY_REG_BACKLIGHT, brightness);
}

esp_err_t topway_clear_screen(uint16_t color)
{
    /* Fill entire screen with color */
    return topway_fill_rect(0, 0, 800, 480, color);
}

esp_err_t topway_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
    /* Use CMD_DRAW_RECT with fill: 5A A5 25 [x1_h] [x1_l] [y1_h] [y1_l] [x2_h] [x2_l] [y2_h] [y2_l] [color_h] [color_l] */
    uint8_t cmd[12] = {
        TOPWAY_FRAME_HEAD,
        TOPWAY_FRAME_TAIL,
        0x25,  /* Draw filled rectangle */
        (x >> 8) & 0xFF, x & 0xFF,
        (y >> 8) & 0xFF, y & 0xFF,
        ((x + w - 1) >> 8) & 0xFF, (x + w - 1) & 0xFF,
        ((y + h - 1) >> 8) & 0xFF, (y + h - 1) & 0xFF,
        (color >> 8) & 0xFF, color & 0xFF
    };
    return send_raw(cmd, sizeof(cmd));
}

esp_err_t topway_draw_line(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color)
{
    uint8_t cmd[12] = {
        TOPWAY_FRAME_HEAD,
        TOPWAY_FRAME_TAIL,
        0x22,  /* Draw line */
        (x1 >> 8) & 0xFF, x1 & 0xFF,
        (y1 >> 8) & 0xFF, y1 & 0xFF,
        (x2 >> 8) & 0xFF, x2 & 0xFF,
        (y2 >> 8) & 0xFF, y2 & 0xFF,
        (color >> 8) & 0xFF, color & 0xFF
    };
    return send_raw(cmd, sizeof(cmd));
}

esp_err_t topway_draw_text(uint16_t x, uint16_t y, const char *text, uint16_t color, uint8_t font_size)
{
    if (!text) return ESP_ERR_INVALID_ARG;
    
    size_t len = strlen(text);
    if (len > 200) len = 200;
    
    /* Text display with position: 5A A5 24 [x] [y] [color] [size] [text...] */
    uint8_t cmd[256];
    cmd[0] = TOPWAY_FRAME_HEAD;
    cmd[1] = TOPWAY_FRAME_TAIL;
    cmd[2] = 0x24;  /* Display text at position */
    cmd[3] = (x >> 8) & 0xFF;
    cmd[4] = x & 0xFF;
    cmd[5] = (y >> 8) & 0xFF;
    cmd[6] = y & 0xFF;
    cmd[7] = (color >> 8) & 0xFF;
    cmd[8] = color & 0xFF;
    cmd[9] = font_size;
    memcpy(&cmd[10], text, len);
    
    return send_raw(cmd, len + 10);
}
