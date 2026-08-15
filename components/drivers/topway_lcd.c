#include "topway_lcd.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "topway";

static uart_port_t s_uart;
static int s_rts_pin = -1;
static SemaphoreHandle_t s_tx_mux;
static uint32_t s_last_malformed_log_ms;

#define TOPWAY_TOUCH_QUEUE_LEN 16

typedef struct {
    uint32_t vp;
    uint16_t value;
} topway_touch_event_t;

static topway_touch_event_t s_touch_queue[TOPWAY_TOUCH_QUEUE_LEN];
static size_t s_touch_queue_len;

static const uint8_t s_tail[4] = {
    TOPWAY_PKT_TAIL0, TOPWAY_PKT_TAIL1, TOPWAY_PKT_TAIL2, TOPWAY_PKT_TAIL3
};

static void drain_rx(void);

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

static esp_err_t tx_packet_locked(const uint8_t *payload, size_t len)
{
    esp_err_t err = wait_busy(200);
    if (err != ESP_OK) return err;

    uint8_t hdr = TOPWAY_PKT_HEADER;
    int written = uart_write_bytes(s_uart, &hdr, 1);
    if (written != 1) {
        ESP_LOGW(TAG, "UART TX failed while writing packet header (%d)", written);
        return ESP_FAIL;
    }
    written = uart_write_bytes(s_uart, payload, len);
    if (written != (int)len) {
        ESP_LOGW(TAG, "UART TX failed while writing packet payload (%d/%u)",
                 written, (unsigned)len);
        return ESP_FAIL;
    }
    written = uart_write_bytes(s_uart, s_tail, sizeof(s_tail));
    if (written != (int)sizeof(s_tail)) {
        ESP_LOGW(TAG, "UART TX failed while writing packet tail (%d/%u)",
                 written, (unsigned)sizeof(s_tail));
        return ESP_FAIL;
    }

    err = uart_wait_tx_done(s_uart, pdMS_TO_TICKS(200));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "UART TX wait failed: %s", esp_err_to_name(err));
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(30));
    return ESP_OK;
}

static esp_err_t send_packet(const uint8_t *payload, size_t len)
{
    if (!payload || len == 0) return ESP_ERR_INVALID_ARG;
    if (!s_tx_mux) return ESP_ERR_INVALID_STATE;

    if (xSemaphoreTake(s_tx_mux, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = tx_packet_locked(payload, len);
    xSemaphoreGive(s_tx_mux);
    return err;
}

static bool ends_with_tail(const uint8_t *buf, size_t len)
{
    return len >= sizeof(s_tail) &&
           memcmp(buf + len - sizeof(s_tail), s_tail, sizeof(s_tail)) == 0;
}

static bool parse_touch_vp_frame(const uint8_t *buf, size_t len, uint32_t *vp, uint16_t *value)
{
    if (!buf || len < 12 || buf[0] != TOPWAY_PKT_HEADER || !ends_with_tail(buf, len)) {
        return false;
    }
    if (buf[1] != TOPWAY_TOUCH_KEY_VP) {
        return false;
    }

    uint32_t parsed_vp = ((uint32_t)buf[2] << 24) |
                         ((uint32_t)buf[3] << 16) |
                         ((uint32_t)buf[4] << 8) |
                         (uint32_t)buf[5];
    uint16_t parsed_value = ((uint16_t)buf[6] << 8) | (uint16_t)buf[7];

    if (vp) *vp = parsed_vp;
    if (value) *value = parsed_value;
    return true;
}

static void queue_touch_event_locked(uint32_t vp, uint16_t value)
{
    if (vp == 0) {
        return;
    }

    if (s_touch_queue_len >= TOPWAY_TOUCH_QUEUE_LEN) {
        memmove(&s_touch_queue[0], &s_touch_queue[1],
                sizeof(s_touch_queue[0]) * (TOPWAY_TOUCH_QUEUE_LEN - 1));
        s_touch_queue_len = TOPWAY_TOUCH_QUEUE_LEN - 1;
    }

    s_touch_queue[s_touch_queue_len].vp = vp;
    s_touch_queue[s_touch_queue_len].value = value;
    s_touch_queue_len++;
}

static bool take_touch_event_locked(uint32_t vp, uint16_t *value)
{
    if (!value) {
        return false;
    }

    for (size_t i = 0; i < s_touch_queue_len; i++) {
        if (s_touch_queue[i].vp == vp) {
            *value = s_touch_queue[i].value;
            if (i + 1 < s_touch_queue_len) {
                memmove(&s_touch_queue[i], &s_touch_queue[i + 1],
                        sizeof(s_touch_queue[0]) * (s_touch_queue_len - i - 1));
            }
            s_touch_queue_len--;
            return true;
        }
    }
    return false;
}

static void log_malformed_frame(const char *reason, const uint8_t *buf, size_t len)
{
    uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if ((now_ms - s_last_malformed_log_ms) < 5000) {
        return;
    }
    s_last_malformed_log_ms = now_ms;

    char hex[3 * 16 + 1] = {0};
    size_t show = len < 16 ? len : 16;
    for (size_t i = 0; i < show; i++) {
        snprintf(hex + i * 3, sizeof(hex) - i * 3, "%02X ", buf[i]);
    }
    ESP_LOGW(TAG, "Malformed RX frame (%s), len=%u, first bytes: %s%s",
             reason, (unsigned)len, hex, len > show ? "..." : "");
}

static esp_err_t read_frame_locked(uint8_t expected_cmd, uint8_t *resp, size_t resp_sz,
                                   size_t *out_len, uint32_t timeout_ms)
{
    if (!resp || resp_sz < 6 || !out_len) return ESP_ERR_INVALID_ARG;

    *out_len = 0;
    size_t len = 0;
    TickType_t start = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    bool in_frame = false;
    uint32_t bytes_seen = 0;

    while ((xTaskGetTickCount() - start) < timeout_ticks) {
        uint8_t b = 0;
        int got = uart_read_bytes(s_uart, &b, 1, pdMS_TO_TICKS(20));
        if (got <= 0) {
            continue;
        }
        bytes_seen++;
        if ((bytes_seen % 32) == 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }

        if (!in_frame) {
            if (b != TOPWAY_PKT_HEADER) {
                continue;
            }
            in_frame = true;
            len = 0;
            resp[len++] = b;
            continue;
        }

        if (len >= resp_sz) {
            log_malformed_frame("oversize/no tail", resp, len);
            in_frame = false;
            len = 0;
            if (b == TOPWAY_PKT_HEADER) {
                in_frame = true;
                resp[len++] = b;
            }
            continue;
        }
        resp[len++] = b;

        if (ends_with_tail(resp, len)) {
            if (expected_cmd == 0 || resp[1] == expected_cmd) {
                *out_len = len;
                return ESP_OK;
            }
            uint32_t vp = 0;
            uint16_t value = 0;
            if (parse_touch_vp_frame(resp, len, &vp, &value)) {
                queue_touch_event_locked(vp, value);
                in_frame = false;
                len = 0;
                continue;
            }
            log_malformed_frame("unexpected command", resp, len);
            in_frame = false;
            len = 0;
        }
    }

    if (in_frame && len > 0) {
        log_malformed_frame("partial timeout", resp, len);
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t transact_packet(const uint8_t *payload, size_t payload_len, uint8_t expected_cmd,
                                 uint8_t *resp, size_t resp_sz, size_t *out_len,
                                 uint32_t timeout_ms)
{
    if (!payload || payload_len == 0 || !resp || !out_len) return ESP_ERR_INVALID_ARG;
    if (!s_tx_mux) return ESP_ERR_INVALID_STATE;

    if (xSemaphoreTake(s_tx_mux, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    drain_rx();
    esp_err_t err = tx_packet_locked(payload, payload_len);
    if (err == ESP_OK) {
        err = read_frame_locked(expected_cmd, resp, resp_sz, out_len, timeout_ms);
    }
    xSemaphoreGive(s_tx_mux);
    return err;
}

static esp_err_t read_any_frame_locked(uint8_t *resp, size_t resp_sz, size_t *out_len)
{
    return read_frame_locked(0, resp, resp_sz, out_len, 30);
}

static void drain_rx(void)
{
    uint8_t frame[32];
    size_t len = 0;

    for (int i = 0; i < 8; i++) {
        size_t buffered = 0;
        if (uart_get_buffered_data_len(s_uart, &buffered) != ESP_OK || buffered == 0) {
            break;
        }
        if (read_any_frame_locked(frame, sizeof(frame), &len) != ESP_OK) {
            break;
        }
        uint32_t vp = 0;
        uint16_t value = 0;
        if (parse_touch_vp_frame(frame, len, &vp, &value)) {
            queue_touch_event_locked(vp, value);
        }
    }
}


esp_err_t topway_init(const topway_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;

    s_uart = config->uart_port;
    s_rts_pin = config->rts_pin;
    s_touch_queue_len = 0;

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
    if (config->rx_pin >= 0) {
        gpio_set_pull_mode(config->rx_pin, GPIO_PULLUP_ONLY);
    }

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
    for (int attempt = 1; attempt <= 3; attempt++) {
        ESP_LOGI(TAG, "Handshake attempt %d/3...", attempt);
        err = topway_handshake();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Handshake OK - display ready");
            break;
        }
        ESP_LOGW(TAG, "Handshake attempt %d failed, retrying in 1s...", attempt);
        vTaskDelay(pdMS_TO_TICKS(1000));
        drain_rx();
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Handshake not ready yet; UART stays active for UI retry");
        ESP_LOGW(TAG, "  Check: ESP TX GPIO -> Topway RX, Topway TX -> ESP RX GPIO");
        ESP_LOGW(TAG, "  Current config: ESP GPIO%d TX -> display RX, display TX -> ESP GPIO%d RX",
                 config->tx_pin, config->rx_pin);
        if (config->rx_pin == 20 || config->tx_pin == 20) {
            ESP_LOGW(TAG, "  GPIO20 is ESP32-S3 USB D+; do not connect native USB while using it as UART RX/TX");
        }
    }

    return ESP_OK;
}

esp_err_t topway_deinit(void)
{
    if (s_tx_mux) {
        vSemaphoreDelete(s_tx_mux);
        s_tx_mux = NULL;
    }
    s_touch_queue_len = 0;
    return uart_driver_delete(s_uart);
}

esp_err_t topway_set_pins(int tx_pin, int rx_pin)
{
    if (!s_tx_mux) return ESP_ERR_INVALID_STATE;
    if (tx_pin < 0 || rx_pin < 0) return ESP_ERR_INVALID_ARG;

    if (xSemaphoreTake(s_tx_mux, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    uart_wait_tx_done(s_uart, pdMS_TO_TICKS(200));
    uart_flush_input(s_uart);
    esp_err_t err = uart_set_pin(s_uart, tx_pin, rx_pin,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err == ESP_OK) {
        gpio_set_pull_mode(rx_pin, GPIO_PULLUP_ONLY);
        ESP_LOGI(TAG, "Topway UART pins changed: ESP GPIO%d TX -> display RX, display TX -> ESP GPIO%d RX",
                 tx_pin, rx_pin);
    }

    xSemaphoreGive(s_tx_mux);
    return err;
}

esp_err_t topway_handshake(void)
{
    uint8_t cmd = TOPWAY_CMD_HAND_SHAKE;
    uint8_t resp[64] = {0};
    size_t len = 0;
    esp_err_t err = transact_packet(&cmd, 1, TOPWAY_CMD_HAND_SHAKE,
                                    resp, sizeof(resp), &len, 1000);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Handshake: no valid response (%s)", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Handshake: got %u bytes: %02X %02X %02X %02X %02X %02X %02X %02X%s",
             (unsigned)len, resp[0], resp[1], resp[2], resp[3],
             len > 4 ? resp[4] : 0, len > 5 ? resp[5] : 0,
             len > 6 ? resp[6] : 0, len > 7 ? resp[7] : 0,
             len > 8 ? " ..." : "");

    size_t payload_end = len >= sizeof(s_tail) ? len - sizeof(s_tail) : len;
    for (size_t i = 2; i < payload_end; i++) {
        if (resp[i] == 0x00) {
            ESP_LOGI(TAG, "Display: %s", (const char *)&resp[2]);
            break;
        }
    }

    return ESP_OK;
}

esp_err_t topway_dump_rx(uint32_t duration_ms)
{
    if (!s_tx_mux) return ESP_ERR_INVALID_STATE;
    if (duration_ms == 0) duration_ms = 1000;
    if (duration_ms > 5000) duration_ms = 5000;

    if (xSemaphoreTake(s_tx_mux, pdMS_TO_TICKS(250)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    uint8_t buf[32];
    size_t total = 0;
    TickType_t start = xTaskGetTickCount();
    TickType_t duration = pdMS_TO_TICKS(duration_ms);

    ESP_LOGI(TAG, "Topway RX dump started for %lu ms", (unsigned long)duration_ms);
    while ((xTaskGetTickCount() - start) < duration) {
        int got = uart_read_bytes(s_uart, buf, sizeof(buf), pdMS_TO_TICKS(50));
        if (got <= 0) {
            continue;
        }
        total += (size_t)got;

        char hex[3 * 32 + 1] = {0};
        for (int i = 0; i < got; i++) {
            snprintf(hex + i * 3, sizeof(hex) - i * 3, "%02X ", buf[i]);
        }
        ESP_LOGI(TAG, "RX[%d]: %s", got, hex);
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    xSemaphoreGive(s_tx_mux);
    ESP_LOGI(TAG, "Topway RX dump complete: %u byte(s)", (unsigned)total);
    return total > 0 ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t topway_tx_burst(uint32_t duration_ms)
{
    if (!s_tx_mux) return ESP_ERR_INVALID_STATE;
    if (duration_ms == 0) duration_ms = 1000;
    if (duration_ms > 10000) duration_ms = 10000;

    if (xSemaphoreTake(s_tx_mux, pdMS_TO_TICKS(250)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = wait_busy(200);
    if (err != ESP_OK) {
        xSemaphoreGive(s_tx_mux);
        return err;
    }

    uint8_t pattern[64];
    memset(pattern, 0x55, sizeof(pattern));

    size_t total = 0;
    TickType_t start = xTaskGetTickCount();
    TickType_t duration = pdMS_TO_TICKS(duration_ms);

    ESP_LOGI(TAG, "Topway TX burst started for %lu ms", (unsigned long)duration_ms);
    while ((xTaskGetTickCount() - start) < duration) {
        int written = uart_write_bytes(s_uart, pattern, sizeof(pattern));
        if (written != (int)sizeof(pattern)) {
            ESP_LOGW(TAG, "UART TX burst write failed (%d/%u)",
                     written, (unsigned)sizeof(pattern));
            err = ESP_FAIL;
            break;
        }
        total += (size_t)written;
        err = uart_wait_tx_done(s_uart, pdMS_TO_TICKS(100));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "UART TX burst wait failed: %s", esp_err_to_name(err));
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    xSemaphoreGive(s_tx_mux);
    ESP_LOGI(TAG, "Topway TX burst complete: %u byte(s)", (unsigned)total);
    return err == ESP_OK && total > 0 ? ESP_OK : err;
}

esp_err_t topway_read_version(char *out, size_t out_sz)
{
    uint8_t cmd = TOPWAY_CMD_READ_VERSION;
    uint8_t resp[32] = {0};
    size_t len = 0;
    esp_err_t err = transact_packet(&cmd, 1, TOPWAY_CMD_READ_VERSION,
                                    resp, sizeof(resp), &len, 500);
    if (err != ESP_OK) {
        return err;
    }

    if (out && out_sz > 0) {
        size_t payload_end = len >= sizeof(s_tail) ? len - sizeof(s_tail) : len;
        size_t i;
        for (i = 0; i < out_sz - 1 && i + 2 < payload_end && resp[2 + i] != 0x00; i++) {
            out[i] = resp[2 + i];
        }
        out[i] = '\0';
    }
    return ESP_OK;
}

esp_err_t topway_read_page_id(uint16_t *page_id)
{
    uint8_t cmd = TOPWAY_CMD_READ_PG_ID;
    uint8_t resp[16] = {0};
    size_t len = 0;
    esp_err_t err = transact_packet(&cmd, 1, TOPWAY_CMD_READ_PG_ID,
                                    resp, sizeof(resp), &len, 500);
    if (err != ESP_OK) {
        return err;
    }
    if (len < 8) return ESP_ERR_INVALID_RESPONSE;
    if (page_id) *page_id = ((uint16_t)resp[2] << 8) | resp[3];
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

esp_err_t topway_usb_unlock(const char *password)
{
    if (!password) return ESP_ERR_INVALID_ARG;
    size_t len = strlen(password);
    if (len == 0 || len > 127) return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)password[i];
        if (c < 0x20 || c > 0x7E) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    uint8_t pkt[1 + 127 + 1];
    pkt[0] = TOPWAY_CMD_U_DRV_UNLOCK;
    memcpy(&pkt[1], password, len);
    pkt[1 + len] = 0x00;
    return send_packet(pkt, 1 + len + 1);
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

    uint8_t resp[16] = {0};
    size_t len = 0;
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < 2; attempt++) {
        err = transact_packet(pkt, sizeof(pkt), TOPWAY_CMD_N16_READ,
                              resp, sizeof(resp), &len, 800);
        if (err == ESP_OK) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (err != ESP_OK) return err;
    if (len < 8) return ESP_ERR_INVALID_RESPONSE;
    *value = ((uint16_t)resp[2] << 8) | resp[3];
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
    out[0] = '\0';

    uint8_t pkt[5] = {
        TOPWAY_CMD_STR_READ,
        (addr >> 24) & 0xFF, (addr >> 16) & 0xFF, (addr >> 8) & 0xFF, addr & 0xFF,
    };

    uint8_t resp[140] = {0};
    size_t len = 0;
    esp_err_t err = transact_packet(pkt, sizeof(pkt), TOPWAY_CMD_STR_READ,
                                    resp, sizeof(resp), &len, 500);
    if (err != ESP_OK) {
        return err;
    }

    size_t payload_end = len >= sizeof(s_tail) ? len - sizeof(s_tail) : len;
    size_t i;
    for (i = 0; i < out_sz - 1 && i + 2 < payload_end && resp[2 + i] != 0x00; i++) {
        out[i] = resp[2 + i];
    }
    out[i] = '\0';

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
    if (data_len > 510 || data_len != (size_t)length) return ESP_ERR_INVALID_ARG;

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

bool topway_take_touch_event(uint32_t vp, uint16_t *value)
{
    if (!value || !s_tx_mux) {
        return false;
    }
    if (xSemaphoreTake(s_tx_mux, pdMS_TO_TICKS(20)) != pdTRUE) {
        return false;
    }
    bool found = take_touch_event_locked(vp, value);
    xSemaphoreGive(s_tx_mux);
    return found;
}

void topway_process_touch_events(void)
{
    if (!s_touch_callback) return;
    if (!s_tx_mux) return;

    if (xSemaphoreTake(s_tx_mux, 0) != pdTRUE) {
        return;
    }

    uint32_t vps[8];
    uint16_t values[8];
    size_t event_count = 0;
    uint8_t frame[32];
    size_t len = 0;

    while (event_count < (sizeof(vps) / sizeof(vps[0])) &&
           read_any_frame_locked(frame, sizeof(frame), &len) == ESP_OK) {
        uint32_t vp = 0;
        uint16_t value = 0;
        if (parse_touch_vp_frame(frame, len, &vp, &value)) {
            queue_touch_event_locked(vp, value);
            vps[event_count] = vp;
            values[event_count] = value;
            event_count++;
        }
    }

    xSemaphoreGive(s_tx_mux);

    for (size_t i = 0; i < event_count && s_touch_callback; i++) {
        s_touch_callback(vps[i], values[i]);
    }
}
