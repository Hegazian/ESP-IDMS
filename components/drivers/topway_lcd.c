#include "topway_lcd.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "topway";

static uart_port_t s_uart;
static int s_rts_pin = -1;
static SemaphoreHandle_t s_tx_mux;

static esp_err_t wait_busy(uint32_t timeout_ms)
{
    if (s_rts_pin < 0) return ESP_OK;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (gpio_get_level(s_rts_pin) == 0) {
        if (xTaskGetTickCount() >= deadline) {
            ESP_LOGW(TAG, "BUSY timeout");
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return ESP_OK;
}

static esp_err_t send_packet(const uint8_t *payload, size_t len)
{
    if (!payload || len == 0) return ESP_ERR_INVALID_ARG;
    if (!s_tx_mux) return ESP_ERR_INVALID_STATE;

    esp_err_t err = wait_busy(200);
    if (err != ESP_OK) return err;

    if (xSemaphoreTake(s_tx_mux, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    uint8_t hdr = TOPWAY_PKT_HEADER;
    uart_write_bytes(s_uart, &hdr, 1);
    uart_write_bytes(s_uart, payload, len);
    uint8_t tail[4] = { TOPWAY_PKT_TAIL0, TOPWAY_PKT_TAIL1, TOPWAY_PKT_TAIL2, TOPWAY_PKT_TAIL3 };
    uart_write_bytes(s_uart, tail, 4);

    uart_wait_tx_done(s_uart, pdMS_TO_TICKS(200));
    xSemaphoreGive(s_tx_mux);

    vTaskDelay(pdMS_TO_TICKS(30));
    return ESP_OK;
}

static void drain_rx(void)
{
    uint8_t tmp[64];
    while (uart_read_bytes(s_uart, tmp, sizeof(tmp), pdMS_TO_TICKS(10)) > 0) {
    }
}

esp_err_t topway_init(const topway_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;

    s_uart = config->uart_port;
    s_rts_pin = config->rts_pin;

    uart_config_t uart_config = {
        .baud_rate = config->baud_rate ? config->baud_rate : 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };

    ESP_ERROR_CHECK(uart_param_config(s_uart, &uart_config));
    ESP_ERROR_CHECK(uart_driver_install(s_uart, 512, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_set_pin(s_uart, config->tx_pin, config->rx_pin,
                                  UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    if (s_rts_pin >= 0) {
        gpio_config_t io = {
            .pin_bit_mask = 1ULL << s_rts_pin,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
        };
        gpio_config(&io);
    }

    s_tx_mux = xSemaphoreCreateMutex();
    if (!s_tx_mux) {
        uart_driver_delete(s_uart);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Topway LCD: UART%d @ %lu baud (TX=%d, RX=%d, RTS=%d)",
             s_uart, (unsigned long)config->baud_rate,
             config->tx_pin, config->rx_pin, s_rts_pin);

    vTaskDelay(pdMS_TO_TICKS(3000));
    drain_rx();

    esp_err_t err = ESP_FAIL;
    for (int attempt = 1; attempt <= 5; attempt++) {
        ESP_LOGI(TAG, "Handshake attempt %d/5...", attempt);
        err = topway_handshake();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Handshake OK — display ready");
            break;
        }
        ESP_LOGW(TAG, "Handshake attempt %d failed, retrying in 1s...", attempt);
        vTaskDelay(pdMS_TO_TICKS(1000));
        drain_rx();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Handshake failed after 5 attempts");
        ESP_LOGE(TAG, "  Check: RS232-TTL wiring (TX<->RX cross), baud rate, VCC");
        ESP_LOGE(TAG, "  Display needs 12V on VDD pin, RS232 converter needs 3.3-5V");
        ESP_LOGE(TAG, "  Try 9600 baud if jumpers changed (JP1,JP8 close; JP2,JP7 open)");
        topway_deinit();
        return err;
    }

    return ESP_OK;
}

esp_err_t topway_deinit(void)
{
    if (s_tx_mux) {
        vSemaphoreDelete(s_tx_mux);
        s_tx_mux = NULL;
    }
    return uart_driver_delete(s_uart);
}

esp_err_t topway_handshake(void)
{
    drain_rx();
    uint8_t cmd = TOPWAY_CMD_HAND_SHAKE;
    esp_err_t err = send_packet(&cmd, 1);
    if (err != ESP_OK) return err;

    uint8_t resp[64] = {0};
    int len = uart_read_bytes(s_uart, resp, sizeof(resp), pdMS_TO_TICKS(1000));
    if (len <= 0) {
        ESP_LOGW(TAG, "Handshake: no response (len=%d)", len);
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "Handshake: got %d bytes: %02X %02X %02X %02X %02X %02X %02X %02X%s",
             len, resp[0], resp[1], resp[2], resp[3],
             len > 4 ? resp[4] : 0, len > 5 ? resp[5] : 0,
             len > 6 ? resp[6] : 0, len > 7 ? resp[7] : 0,
             len > 8 ? " ..." : "");

    if (len < 4) return ESP_ERR_TIMEOUT;

    if (resp[0] == TOPWAY_PKT_HEADER && resp[1] == TOPWAY_CMD_HAND_SHAKE) {
        for (int i = 2; i < len; i++) {
            if (resp[i] == 0x00) {
                ESP_LOGI(TAG, "Display: %s", (const char *)&resp[2]);
                break;
            }
        }
        drain_rx();
        return ESP_OK;
    }

    ESP_LOGW(TAG, "Unexpected handshake response (header=0x%02X, cmd=0x%02X)", resp[0], resp[1]);
    drain_rx();
    return ESP_FAIL;
}

esp_err_t topway_read_version(char *out, size_t out_sz)
{
    uint8_t cmd = TOPWAY_CMD_READ_VERSION;
    esp_err_t err = send_packet(&cmd, 1);
    if (err != ESP_OK) return err;

    uint8_t resp[32] = {0};
    int len = uart_read_bytes(s_uart, resp, sizeof(resp), pdMS_TO_TICKS(500));
    if (len < 4 || resp[0] != TOPWAY_PKT_HEADER || resp[1] != TOPWAY_CMD_READ_VERSION) {
        return ESP_ERR_TIMEOUT;
    }

    if (out && out_sz > 0) {
        size_t i;
        for (i = 0; i < out_sz - 1 && i + 2 < (size_t)len && resp[2 + i] != 0x00; i++) {
            out[i] = resp[2 + i];
        }
        out[i] = '\0';
    }
    drain_rx();
    return ESP_OK;
}

esp_err_t topway_read_page_id(uint16_t *page_id)
{
    uint8_t cmd = TOPWAY_CMD_READ_PG_ID;
    esp_err_t err = send_packet(&cmd, 1);
    if (err != ESP_OK) return err;

    uint8_t resp[16] = {0};
    int len = uart_read_bytes(s_uart, resp, sizeof(resp), pdMS_TO_TICKS(500));
    if (len < 7 || resp[0] != TOPWAY_PKT_HEADER || resp[1] != TOPWAY_CMD_READ_PG_ID) {
        return ESP_ERR_TIMEOUT;
    }
    if (page_id) *page_id = ((uint16_t)resp[2] << 8) | resp[3];
    drain_rx();
    return ESP_OK;
}

esp_err_t topway_set_sys_config(uint8_t baud_code, uint8_t touch_cfg)
{
    uint8_t pkt[7] = {
        TOPWAY_CMD_SET_SYS_CONFIG,
        0x55, 0xAA, 0x5A, 0xA5,
        baud_code,
        touch_cfg,
    };
    return send_packet(pkt, sizeof(pkt));
}

esp_err_t topway_select_project(uint8_t prj_id)
{
    uint8_t pkt[2] = { TOPWAY_CMD_SEL_PROJECT, prj_id };
    return send_packet(pkt, sizeof(pkt));
}

esp_err_t topway_set_backlight(uint8_t level)
{
    if (level > 0x3F) level = 0x3F;
    uint8_t pkt[2] = { TOPWAY_CMD_BACKLIGHT_CTRL, level };
    return send_packet(pkt, sizeof(pkt));
}

esp_err_t topway_screen_saver(uint16_t timeout_s, uint8_t dim_level)
{
    if (dim_level > 0x3F) dim_level = 0x3F;
    uint8_t pkt[4] = {
        TOPWAY_CMD_SCREEN_SAVER,
        (timeout_s >> 8) & 0xFF, timeout_s & 0xFF,
        dim_level,
    };
    return send_packet(pkt, sizeof(pkt));
}

esp_err_t topway_buzzer_ctrl(uint8_t loops, uint8_t t1, uint8_t t2, uint8_t freq1, uint8_t freq2)
{
    uint8_t pkt[6] = { TOPWAY_CMD_BUZZER_CTRL, loops, t1, t2, freq1, freq2 };
    return send_packet(pkt, sizeof(pkt));
}

esp_err_t topway_disp_page(uint16_t page_id)
{
    uint8_t pkt[3] = {
        TOPWAY_CMD_DISP_PAGE,
        (page_id >> 8) & 0xFF,
        page_id & 0xFF,
    };
    return send_packet(pkt, sizeof(pkt));
}

esp_err_t topway_set_element_fg(uint8_t element, uint16_t page_id, uint8_t element_id, uint16_t color)
{
    uint8_t pkt[8] = {
        TOPWAY_CMD_SET_ELEMENT_FG,
        element,
        (page_id >> 8) & 0xFF, page_id & 0xFF,
        element_id,
        0x00,
        (color >> 8) & 0xFF, color & 0xFF,
    };
    return send_packet(pkt, sizeof(pkt));
}

esp_err_t topway_set_element_bg(uint8_t element, uint16_t page_id, uint8_t element_id, uint8_t mode, uint16_t color)
{
    uint8_t pkt[8] = {
        TOPWAY_CMD_SET_ELEMENT_BG,
        element,
        (page_id >> 8) & 0xFF, page_id & 0xFF,
        element_id,
        mode,
        (color >> 8) & 0xFF, color & 0xFF,
    };
    return send_packet(pkt, sizeof(pkt));
}

esp_err_t topway_set_codepage(uint8_t country, uint8_t codepage)
{
    uint8_t pkt[3] = { TOPWAY_CMD_SET_CODEPAGE, country, codepage };
    return send_packet(pkt, sizeof(pkt));
}

esp_err_t topway_suspend_refresh(bool suspend)
{
    uint8_t pkt[6] = {
        TOPWAY_CMD_SUSPEND_REFRESH,
        0x55, 0xAA, 0x5A, 0xA5,
        suspend ? 0x01 : 0x00,
    };
    return send_packet(pkt, sizeof(pkt));
}

esp_err_t topway_n16_write(uint32_t addr, uint16_t value)
{
    uint8_t pkt[7] = {
        TOPWAY_CMD_N16_WRITE,
        (addr >> 24) & 0xFF, (addr >> 16) & 0xFF, (addr >> 8) & 0xFF, addr & 0xFF,
        (value >> 8) & 0xFF, value & 0xFF,
    };
    return send_packet(pkt, sizeof(pkt));
}

esp_err_t topway_n16_read(uint32_t addr, uint16_t *value)
{
    if (!value) return ESP_ERR_INVALID_ARG;

    uint8_t pkt[5] = {
        TOPWAY_CMD_N16_READ,
        (addr >> 24) & 0xFF, (addr >> 16) & 0xFF, (addr >> 8) & 0xFF, addr & 0xFF,
    };
    
    esp_err_t err = send_packet(pkt, sizeof(pkt));
    if (err != ESP_OK) return err;

    uint8_t resp[16] = {0};
    int len = uart_read_bytes(s_uart, resp, sizeof(resp), pdMS_TO_TICKS(500));
    
    if (len < 6 || resp[0] != TOPWAY_PKT_HEADER || resp[1] != TOPWAY_CMD_N16_READ) {
        return ESP_ERR_TIMEOUT;
    }
    *value = ((uint16_t)resp[2] << 8) | resp[3];
    drain_rx();
    return ESP_OK;
}

esp_err_t topway_n16_fill(uint32_t addr, uint16_t length, uint16_t value)
{
    uint8_t pkt[9] = {
        TOPWAY_CMD_N16_FILL,
        (addr >> 24) & 0xFF, (addr >> 16) & 0xFF, (addr >> 8) & 0xFF, addr & 0xFF,
        (length >> 8) & 0xFF, length & 0xFF,
        (value >> 8) & 0xFF, value & 0xFF,
    };
    return send_packet(pkt, sizeof(pkt));
}

esp_err_t topway_n32_write(uint32_t addr, uint32_t value)
{
    uint8_t pkt[9] = {
        TOPWAY_CMD_N32_WRITE,
        (addr >> 24) & 0xFF, (addr >> 16) & 0xFF, (addr >> 8) & 0xFF, addr & 0xFF,
        (value >> 24) & 0xFF, (value >> 16) & 0xFF,
        (value >> 8) & 0xFF, value & 0xFF,
    };
    return send_packet(pkt, sizeof(pkt));
}

esp_err_t topway_n64_write(uint32_t addr, uint64_t value)
{
    uint8_t pkt[13] = {
        TOPWAY_CMD_N64_WRITE,
        (addr >> 24) & 0xFF, (addr >> 16) & 0xFF, (addr >> 8) & 0xFF, addr & 0xFF,
        (value >> 56) & 0xFF, (value >> 48) & 0xFF,
        (value >> 40) & 0xFF, (value >> 32) & 0xFF,
        (value >> 24) & 0xFF, (value >> 16) & 0xFF,
        (value >> 8) & 0xFF, value & 0xFF,
    };
    return send_packet(pkt, sizeof(pkt));
}

esp_err_t topway_str_write(uint32_t addr, const char *str)
{
    if (!str) return ESP_ERR_INVALID_ARG;
    size_t slen = strlen(str);
    if (slen > 127) slen = 127;

    uint8_t pkt[5 + 128 + 1];
    pkt[0] = TOPWAY_CMD_STR_WRITE;
    pkt[1] = (addr >> 24) & 0xFF;
    pkt[2] = (addr >> 16) & 0xFF;
    pkt[3] = (addr >> 8) & 0xFF;
    pkt[4] = addr & 0xFF;
    memcpy(&pkt[5], str, slen);
    pkt[5 + slen] = 0x00;  /* null terminator required by Topway protocol */

    return send_packet(pkt, 5 + slen + 1);
}

esp_err_t topway_str_read(uint32_t addr, char *out, size_t out_sz)
{
    if (!out || out_sz == 0) return ESP_ERR_INVALID_ARG;

    uint8_t pkt[5] = {
        TOPWAY_CMD_STR_READ,
        (addr >> 24) & 0xFF, (addr >> 16) & 0xFF, (addr >> 8) & 0xFF, addr & 0xFF,
    };

    esp_err_t err = send_packet(pkt, sizeof(pkt));
    if (err != ESP_OK) return err;

    uint8_t resp[140] = {0};
    int len = uart_read_bytes(s_uart, resp, sizeof(resp), pdMS_TO_TICKS(500));

    if (len < 4 || resp[0] != TOPWAY_PKT_HEADER || resp[1] != TOPWAY_CMD_STR_READ) {
        return ESP_ERR_TIMEOUT;
    }

    /* Copy string data (starts at offset 2) until null terminator or max length */
    size_t i;
    for (i = 0; i < out_sz - 1 && i + 2 < (size_t)len && resp[2 + i] != 0x00; i++) {
        out[i] = resp[2 + i];
    }
    out[i] = '\0';

    drain_rx();
    return ESP_OK;
}

esp_err_t topway_str_fill(uint32_t addr, uint16_t length, const char *str)
{
    if (!str) return ESP_ERR_INVALID_ARG;
    size_t slen = strlen(str);
    if (slen > 127) slen = 127;

    uint8_t pkt[7 + 128];
    pkt[0] = TOPWAY_CMD_STR_FILL;
    pkt[1] = (addr >> 24) & 0xFF;
    pkt[2] = (addr >> 16) & 0xFF;
    pkt[3] = (addr >> 8) & 0xFF;
    pkt[4] = addr & 0xFF;
    pkt[5] = (length >> 8) & 0xFF;
    pkt[6] = length & 0xFF;
    memcpy(&pkt[7], str, slen);

    return send_packet(pkt, 7 + slen);
}

esp_err_t topway_successive_write(uint32_t addr, uint8_t length, const uint8_t *data, size_t data_len)
{
    if (!data || length == 0) return ESP_ERR_INVALID_ARG;

    uint8_t pkt[7 + 510];
    pkt[0] = TOPWAY_CMD_SUCCESSIVE_WRITE;
    pkt[1] = (addr >> 24) & 0xFF;
    pkt[2] = (addr >> 16) & 0xFF;
    pkt[3] = (addr >> 8) & 0xFF;
    pkt[4] = addr & 0xFF;
    pkt[5] = length;
    memcpy(&pkt[6], data, data_len);

    return send_packet(pkt, 6 + data_len);
}

esp_err_t topway_g16_write(uint32_t addr, uint16_t size, const uint16_t *values)
{
    if (!values || size == 0 || size > 1024) return ESP_ERR_INVALID_ARG;

    uint8_t pkt[7 + 2048];
    pkt[0] = TOPWAY_CMD_G16_WRITE;
    pkt[1] = (addr >> 8) & 0xFF;
    pkt[2] = addr & 0xFF;
    pkt[3] = ((addr + size * 2 - 2) >> 8) & 0xFF;
    pkt[4] = (addr + size * 2 - 2) & 0xFF;
    pkt[5] = (size >> 8) & 0xFF;
    pkt[6] = size & 0xFF;
    for (uint16_t i = 0; i < size; i++) {
        pkt[7 + i * 2] = (values[i] >> 8) & 0xFF;
        pkt[7 + i * 2 + 1] = values[i] & 0xFF;
    }

    return send_packet(pkt, 7 + size * 2);
}

esp_err_t topway_g16_write_rotate(uint32_t addr, uint16_t size, uint16_t value)
{
    uint8_t pkt[9] = {
        TOPWAY_CMD_G16_WRITE_ROTATE,
        (addr >> 8) & 0xFF, addr & 0xFF,
        ((addr + size * 2 - 2) >> 8) & 0xFF, (addr + size * 2 - 2) & 0xFF,
        (size >> 8) & 0xFF, size & 0xFF,
        (value >> 8) & 0xFF, value & 0xFF,
    };
    return send_packet(pkt, sizeof(pkt));
}

esp_err_t topway_sys_reg_write(uint32_t addr, uint8_t value)
{
    uint8_t pkt[6] = {
        TOPWAY_CMD_SYS_REG_WRITE,
        (addr >> 24) & 0xFF, (addr >> 16) & 0xFF, (addr >> 8) & 0xFF, addr & 0xFF,
        value,
    };
    return send_packet(pkt, sizeof(pkt));
}

esp_err_t topway_rtc_set(uint8_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t min, uint8_t sec)
{
    uint8_t pkt[7] = {
        TOPWAY_CMD_RTC_SET,
        year, month, day, hour, min, sec,
    };
    return send_packet(pkt, sizeof(pkt));
}

static topway_touch_callback_t s_touch_callback = NULL;

void topway_register_touch_callback(topway_touch_callback_t callback)
{
    s_touch_callback = callback;
}

void topway_process_touch_events(void)
{
    if (!s_touch_callback) return;

    uint8_t buf[16];
    int len;

    /* Non-blocking read of any pending data */
    while ((len = uart_read_bytes(s_uart, buf, sizeof(buf), 0)) > 0) {
        /* Check for touch key event: AA 77 <page_id> <key_id> <tail> */
        for (int i = 0; i < len - 4; i++) {
            if (buf[i] == TOPWAY_PKT_HEADER && buf[i+1] == TOPWAY_TOUCH_KEY_VP) {
                uint8_t page_id = buf[i+2];
                uint8_t key_id = buf[i+3];
                if (s_touch_callback) {
                    s_touch_callback(page_id, key_id);
                }
            }
        }
    }
}
