#include "xpt2046.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "driver/gpio.h"
#include <string.h>

static const char *TAG = "xpt2046";

static spi_device_handle_t s_dev;

static esp_err_t xfer(uint8_t cmd, uint16_t *out12)
{
    if (!out12) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t tx[3] = {cmd, 0x00, 0x00};
    uint8_t rx[3] = {0};

    spi_transaction_t t = {
        .length = 3 * 8,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };

    esp_err_t err = spi_device_polling_transmit(s_dev, &t);
    if (err != ESP_OK) {
        return err;
    }

    uint16_t v = ((uint16_t)rx[1] << 8) | rx[2];
    v >>= 3;
    v &= 0x0FFF;
    *out12 = v;
    return ESP_OK;
}

esp_err_t xpt2046_init(spi_host_device_t host)
{
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 2 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = CONFIG_IDMS_PIN_TOUCH_CS,
        .queue_size = 1,
    };

    esp_err_t err = spi_bus_add_device(host, &devcfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device failed: %s", esp_err_to_name(err));
        return err;
    }

    if (CONFIG_IDMS_PIN_TOUCH_IRQ >= 0) {
        gpio_config_t io = {
            .pin_bit_mask = 1ULL << CONFIG_IDMS_PIN_TOUCH_IRQ,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
        };
        gpio_config(&io);
    }

    ESP_LOGI(TAG, "XPT2046 on CS GPIO%d", CONFIG_IDMS_PIN_TOUCH_CS);
    return ESP_OK;
}

bool xpt2046_read(int16_t *x, int16_t *y, bool *pressed)
{
    if (!x || !y || !pressed) {
        return false;
    }

    uint16_t z1 = 0, z2 = 0;
    if (xfer(0xB1, &z1) != ESP_OK) {
        *pressed = false;
        return false;
    }
    if (xfer(0xC1, &z2) != ESP_OK) {
        *pressed = false;
        return false;
    }

    uint16_t rt = z1 + 4095 - z2;
    *pressed = rt > 350;

    if (!*pressed) {
        return true;
    }

    uint16_t raw_y = 0, raw_x = 0;
    if (xfer(0xD0, &raw_y) != ESP_OK) {
        *pressed = false;
        return false;
    }
    if (xfer(0x90, &raw_x) != ESP_OK) {
        *pressed = false;
        return false;
    }

    *x = (int16_t)raw_x;
    *y = (int16_t)raw_y;
    return true;
}
