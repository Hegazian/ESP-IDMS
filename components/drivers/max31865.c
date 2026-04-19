#include "max31865.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <math.h>

#if CONFIG_IDMS_TEMP_SENSOR_MAX31865

static const char *TAG = "max31865";

#define MAX31865_NUM_SENSORS 2

/* MAX31865 register addresses (write: bit7=0, read: bit7=1) */
#define REG_CONFIG          0x00
#define REG_RTD_MSB         0x01
#define REG_RTD_LSB         0x02
#define REG_H_FAULT_MSB     0x03
#define REG_H_FAULT_LSB     0x04
#define REG_L_FAULT_MSB     0x05
#define REG_L_FAULT_LSB     0x06
#define REG_FAULT_STATUS     0x07

/* Config register bits */
#define CFG_VBIAS_ON        (1 << 7)
#define CFG_AUTO_CONVERSION  (1 << 6)
#define CFG_3WIRE_RTD       (1 << 5)
#define CFG_FAULT_DET_NONE  (0 << 3)
#define CFG_50HZ_FILTER     (1 << 1)

/* PT100 Callendar-Van Dusen coefficients (IEC 751) */
#define RTD_A   3.9083e-3f
#define RTD_B   -5.775e-7f
#define RTD_R0  100.0f

static spi_device_handle_t s_spi[MAX31865_NUM_SENSORS];
static bool s_initialized[MAX31865_NUM_SENSORS];
static int s_count = 0;
static SemaphoreHandle_t s_bus_mutex;

/* Reference resistor value from Kconfig (in ohms, times 10 for fixed-point) */
#define RREF_OHMS  ((float)CONFIG_IDMS_MAX31865_RREF_X10 / 10.0f)

/*
 * SPI bus sharing: The LCD panel IO uses DMA (interrupt-driven) transfers.
 * MAX31865 must use interrupt-driven SPI (queue_trans) so it doesn't
 * conflict with ongoing LCD DMA. A mutex serializes all MAX31865 SPI access.
 */

static esp_err_t spi_xfer(spi_device_handle_t dev, spi_transaction_t *t)
{
    spi_transaction_t *ret_trans;
    esp_err_t err;

    if (xSemaphoreTake(s_bus_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    err = spi_device_queue_trans(dev, t, pdMS_TO_TICKS(100));
    if (err != ESP_OK) {
        xSemaphoreGive(s_bus_mutex);
        return err;
    }

    err = spi_device_get_trans_result(dev, &ret_trans, pdMS_TO_TICKS(500));
    xSemaphoreGive(s_bus_mutex);
    return err;
}

static esp_err_t write_register(spi_device_handle_t dev, uint8_t reg, uint8_t val)
{
    uint8_t tx[2] = { (uint8_t)(reg & 0x7F), val };
    spi_transaction_t t = {
        .length = 16,
        .tx_buffer = tx,
    };
    return spi_xfer(dev, &t);
}

static esp_err_t read_registers(spi_device_handle_t dev, uint8_t start_reg, uint8_t *out, int len)
{
    uint8_t tx[8] = {0};
    uint8_t rx[8] = {0};
    tx[0] = (uint8_t)(start_reg | 0x80);
    for (int i = 1; i <= len; i++) {
        tx[i] = 0xFF;
    }
    spi_transaction_t t = {
        .length = (8 * (1 + len)),
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    esp_err_t err = spi_xfer(dev, &t);
    if (err != ESP_OK) {
        return err;
    }
    memcpy(out, rx + 1, len);
    return ESP_OK;
}

static uint16_t read_rtd(spi_device_handle_t dev)
{
    uint8_t buf[2] = {0};
    if (read_registers(dev, REG_RTD_MSB, buf, 2) != ESP_OK) {
        return 0;
    }
    return ((uint16_t)buf[0] << 8) | buf[1];
}

static uint8_t read_fault(spi_device_handle_t dev)
{
    uint8_t fault = 0;
    read_registers(dev, REG_FAULT_STATUS, &fault, 1);
    return fault;
}

static void clear_fault(spi_device_handle_t dev)
{
    write_register(dev, REG_CONFIG,
        CFG_VBIAS_ON | CFG_AUTO_CONVERSION | CFG_50HZ_FILTER | (1 << 2));
    write_register(dev, REG_CONFIG,
        CFG_VBIAS_ON | CFG_AUTO_CONVERSION | CFG_50HZ_FILTER);
}

static float rtd_to_celsius(uint16_t rtd_raw)
{
    if (rtd_raw == 0) return -999.0f;

    float rtd = (float)rtd_raw * RREF_OHMS / 32768.0f;

    if (rtd < 10.0f || rtd > 500.0f) return -999.0f;

    float z = rtd / RTD_R0;
    float temp;

    float discriminant = RTD_A * RTD_A - 4.0f * RTD_B * (1.0f - z);
    if (discriminant < 0) {
        temp = (rtd - RTD_R0) / (RTD_R0 * RTD_A);
    } else {
        temp = (-RTD_A + sqrtf(discriminant)) / (2.0f * RTD_B);
    }

    return temp;
}

static spi_host_device_t s_spi_host;

esp_err_t max31865_init(spi_host_device_t spi_host)
{
    s_spi_host = spi_host;
    s_count = 0;
    memset(s_initialized, 0, sizeof(s_initialized));

    s_bus_mutex = xSemaphoreCreateMutex();
    if (!s_bus_mutex) {
        ESP_LOGE(TAG, "Failed to create SPI bus mutex");
        return ESP_ERR_NO_MEM;
    }

    gpio_num_t cs_pins[MAX31865_NUM_SENSORS] = {
        (gpio_num_t)CONFIG_IDMS_MAX31865_CS0,
        (gpio_num_t)CONFIG_IDMS_MAX31865_CS1,
    };

    uint8_t wire_cfg = 0;
#if CONFIG_IDMS_MAX31865_WIRE_3
    wire_cfg = CFG_3WIRE_RTD;
#endif

    uint8_t config = CFG_VBIAS_ON | CFG_AUTO_CONVERSION | wire_cfg | CFG_50HZ_FILTER;

    spi_device_interface_config_t dev_cfg = {
        .mode = 1,
        .clock_speed_hz = CONFIG_IDMS_MAX31865_SPI_CLK_KHZ * 1000,
        .spics_io_num = -1,
        .queue_size = 1,
    };

    for (int i = 0; i < MAX31865_NUM_SENSORS; i++) {
        dev_cfg.spics_io_num = (int)cs_pins[i];

        gpio_config_t io = {
            .pin_bit_mask = 1ULL << cs_pins[i],
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io);
        gpio_set_level(cs_pins[i], 1);

        esp_err_t err = spi_bus_add_device(s_spi_host, &dev_cfg, &s_spi[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to add SPI device for sensor %d (CS=%d): %s",
                     i, (int)cs_pins[i], esp_err_to_name(err));
            continue;
        }

        /* Allow 150ms for first conversion after VBIAS_ON */
        vTaskDelay(pdMS_TO_TICKS(150));

        /* Software reset */
        write_register(s_spi[i], REG_CONFIG, 0x00);
        vTaskDelay(pdMS_TO_TICKS(50));

        /* Configure auto-conversion */
        write_register(s_spi[i], REG_CONFIG, config);
        vTaskDelay(pdMS_TO_TICKS(150));

        /* Clear any initial faults */
        clear_fault(s_spi[i]);

        /* Read and validate RTD */
        uint16_t rtd = read_rtd(s_spi[i]);
        uint8_t fault = read_fault(s_spi[i]);
        if (fault & 0x80) {
            ESP_LOGW(TAG, "Sensor %d (CS=%d): RTD out of range after init (fault=0x%02x, rtd=%u)",
                     i, (int)cs_pins[i], fault, rtd);
        }

        s_initialized[i] = true;
        s_count++;
        ESP_LOGI(TAG, "MAX31865 sensor %d initialized (CS=GPIO%d, RREF=%.1f ohm, RTD=%u)",
                 i, (int)cs_pins[i], RREF_OHMS, rtd);
    }

    if (s_count == 0) {
        ESP_LOGE(TAG, "No MAX31865 sensors initialized");
        return ESP_FAIL;
    }
    return ESP_OK;
}

bool max31865_read_temperature_c(int index, float *out_c)
{
    if (!out_c || index < 0 || index >= MAX31865_NUM_SENSORS || !s_initialized[index]) {
        return false;
    }

    uint8_t fault = read_fault(s_spi[index]);
    if (fault) {
        ESP_LOGD(TAG, "Sensor %d fault: 0x%02x, clearing", index, fault);
        clear_fault(s_spi[index]);
        return false;
    }

    uint16_t rtd_raw = read_rtd(s_spi[index]);
    if (rtd_raw == 0) return false;

    float temp = rtd_to_celsius(rtd_raw);
    if (temp <= -999.0f) return false;

    *out_c = temp;
    return true;
}

int max31865_sensor_count(void)
{
    return s_count;
}

#endif /* CONFIG_IDMS_TEMP_SENSOR_MAX31865 */