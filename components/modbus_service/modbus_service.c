#include "modbus_service.h"
#include "config_store.h"
#include "monitor.h"
#include "mbcontroller.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "modbus";

#define MB_REG_INPUT_START_AREA0    0
#define MB_REG_HOLDING_START_AREA0  0

#pragma pack(push, 1)
typedef struct {
    int16_t t_in;
    int16_t t_out;
    uint16_t current;
    uint16_t faults;
} input_reg_area_t;

typedef struct {
    int16_t dt_alert;
    int16_t dt_high;
    uint16_t reboot_cmd;
} holding_reg_area_t;
#pragma pack(pop)

static input_reg_area_t input_reg;
static holding_reg_area_t holding_reg;

static void modbus_task(void *arg) {
    while (1) {
        idms_metrics_t m;
        monitor_get_metrics(&m);

        input_reg.t_in = (int16_t)(m.t_in_c * 10);
        input_reg.t_out = (int16_t)(m.t_out_c * 10);
        input_reg.current = (uint16_t)(m.current_a * 100);
        
        uint16_t faults = 0;
        if (m.power_fault) faults |= 1;
        if (m.cooling_fault) faults |= 2;
        if (m.delta_alert) faults |= 4;
        input_reg.faults = faults;

        // Check for updates in holding registers written by Master
        if (holding_reg.dt_alert != config_get_dt_alert_threshold()) {
            config_set_dt_alert_threshold(holding_reg.dt_alert);
        }
        if (holding_reg.dt_high != config_get_dt_high_threshold()) {
            config_set_dt_high_threshold(holding_reg.dt_high);
        }
        if (holding_reg.reboot_cmd == 1) {
            ESP_LOGI(TAG, "Reboot requested via Modbus");
            holding_reg.reboot_cmd = 0;
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_restart();
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

#ifndef CONFIG_IDMS_MODBUS_UART_TX_PIN
#define CONFIG_IDMS_MODBUS_UART_TX_PIN 17
#endif
#ifndef CONFIG_IDMS_MODBUS_UART_RX_PIN
#define CONFIG_IDMS_MODBUS_UART_RX_PIN 18
#endif
#ifndef CONFIG_IDMS_MODBUS_UART_RTS_PIN
#define CONFIG_IDMS_MODBUS_UART_RTS_PIN -1
#endif
#ifndef CONFIG_IDMS_MODBUS_MODE_RTU
#define CONFIG_IDMS_MODBUS_MODE_RTU 1
#endif

esp_err_t modbus_service_init(void) {
#if CONFIG_IDMS_MODBUS_ENABLE
    holding_reg.dt_alert = config_get_dt_alert_threshold();
    holding_reg.dt_high = config_get_dt_high_threshold();
    holding_reg.reboot_cmd = 0;

    void* mbc_slave_handler = NULL;
    
#if CONFIG_IDMS_MODBUS_MODE_RTU || CONFIG_IDMS_MODBUS_MODE_BOTH
    ESP_ERROR_CHECK(mbc_slave_init(MB_PORT_SERIAL_SLAVE, &mbc_slave_handler));
    
    mb_communication_info_t comm_info = {
        .mode = MB_MODE_RTU,
        .slave_addr = 1,
        .port = UART_NUM_2,
        .baudrate = 9600,
        .parity = UART_PARITY_DISABLE
    };
    config_get_mb_slave_id(&comm_info.slave_addr);
    
    ESP_ERROR_CHECK(mbc_slave_setup((void*)&comm_info));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM_2, CONFIG_IDMS_MODBUS_UART_TX_PIN, 
                                 CONFIG_IDMS_MODBUS_UART_RX_PIN, 
                                 CONFIG_IDMS_MODBUS_UART_RTS_PIN, 
                                 UART_PIN_NO_CHANGE));
#endif

    mb_register_area_descriptor_t reg_area;

    reg_area.type = MB_PARAM_INPUT;
    reg_area.start_offset = MB_REG_INPUT_START_AREA0;
    reg_area.address = (void*)&input_reg;
    reg_area.size = sizeof(input_reg);
    ESP_ERROR_CHECK(mbc_slave_set_descriptor(reg_area));

    reg_area.type = MB_PARAM_HOLDING;
    reg_area.start_offset = MB_REG_HOLDING_START_AREA0;
    reg_area.address = (void*)&holding_reg;
    reg_area.size = sizeof(holding_reg);
    ESP_ERROR_CHECK(mbc_slave_set_descriptor(reg_area));

    ESP_ERROR_CHECK(mbc_slave_start());

    xTaskCreate(modbus_task, "modbus_update", 4096, NULL, 4, NULL);
    
    ESP_LOGI(TAG, "Modbus initialized");
#endif
    return ESP_OK;
}
