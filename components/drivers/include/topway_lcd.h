#pragma once

#ifndef TOPWAY_LCD_H
#define TOPWAY_LCD_H

#include "driver/uart.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TOPWAY_PKT_HEADER   0xAA
#define TOPWAY_PKT_TAIL0    0xCC
#define TOPWAY_PKT_TAIL1    0x33
#define TOPWAY_PKT_TAIL2    0xC3
#define TOPWAY_PKT_TAIL3    0x3C

#define TOPWAY_ACK_OK       ":>"
#define TOPWAY_ACK_ERR      "!>"

#define TOPWAY_CMD_HAND_SHAKE       0x30
#define TOPWAY_CMD_READ_VERSION     0x31
#define TOPWAY_CMD_READ_PG_ID       0x32
#define TOPWAY_CMD_SET_SYS_CONFIG   0xE0
#define TOPWAY_CMD_SEL_PROJECT      0xE1
#define TOPWAY_CMD_U_DRV_FORMAT     0xE2
#define TOPWAY_CMD_U_DRV_UNLOCK     0xE3
#define TOPWAY_CMD_TOUCH_CALIB      0xE4
#define TOPWAY_CMD_SCREEN_SAVER     0x5E
#define TOPWAY_CMD_BACKLIGHT_CTRL   0x5F
#define TOPWAY_CMD_BUZZER_TOUCH     0x79
#define TOPWAY_CMD_BUZZER_CTRL      0x7A
#define TOPWAY_CMD_FLASH_WRITE      0x90
#define TOPWAY_CMD_FLASH_READ       0x91
#define TOPWAY_CMD_RTC_READ         0x9B
#define TOPWAY_CMD_RTC_SET          0x9C
#define TOPWAY_CMD_USR_BIN_READ     0x93

#define TOPWAY_CMD_DISP_PAGE        0x70
#define TOPWAY_CMD_SET_ELEMENT_FG   0x7E
#define TOPWAY_CMD_SET_ELEMENT_BG   0x7F
#define TOPWAY_CMD_SET_CODEPAGE     0xE7
#define TOPWAY_CMD_SUSPEND_REFRESH  0xE8

#define TOPWAY_CMD_SUCCESSIVE_WRITE 0x82
#define TOPWAY_CMD_SUCCESSIVE_READ  0x83
#define TOPWAY_CMD_BP1_WRITE        0x4B
#define TOPWAY_CMD_BP1_WRITE_COMP   0x4C
#define TOPWAY_CMD_G16_WRITE        0x4D
#define TOPWAY_CMD_G16_WRITE_ROTATE 0x4E
#define TOPWAY_CMD_SYS_REG_WRITE    0x3B
#define TOPWAY_CMD_SYS_REG_READ     0x3C
#define TOPWAY_CMD_STR_WRITE        0x42
#define TOPWAY_CMD_STR_READ         0x43
#define TOPWAY_CMD_STR_FILL         0x46
#define TOPWAY_CMD_N16_WRITE        0x3D
#define TOPWAY_CMD_N16_READ         0x3E
#define TOPWAY_CMD_N16_FILL         0x3F
#define TOPWAY_CMD_N32_WRITE        0x44
#define TOPWAY_CMD_N32_READ         0x45
#define TOPWAY_CMD_N32_FILL         0x47
#define TOPWAY_CMD_N64_WRITE        0x48
#define TOPWAY_CMD_N64_READ         0x49
#define TOPWAY_CMD_N64_FILL         0x4A

#define TOPWAY_TOUCH_RELEASE_COORD  0x72
#define TOPWAY_TOUCH_DOWN_COORD     0x73
#define TOPWAY_TOUCH_KEY_VP         0x77
#define TOPWAY_TOUCH_RELEASE_KEY    0x78
#define TOPWAY_TOUCH_DOWN_KEY       0x79

#define TOPWAY_BAUD_1200    0x00
#define TOPWAY_BAUD_2400    0x01
#define TOPWAY_BAUD_4800    0x02
#define TOPWAY_BAUD_9600    0x03
#define TOPWAY_BAUD_19200   0x04
#define TOPWAY_BAUD_38400   0x05
#define TOPWAY_BAUD_57600   0x06
#define TOPWAY_BAUD_115200  0x07

#define TOPWAY_TOUCH_DISABLE        0x00
#define TOPWAY_TOUCH_DOWN_COORD_CFG 0x05
#define TOPWAY_TOUCH_RELEASE_CFG    0x06
#define TOPWAY_TOUCH_KEY_ID_CFG     0x07

#define TOPWAY_COLOR_BLACK   0x0000
#define TOPWAY_COLOR_WHITE   0xFFFF
#define TOPWAY_COLOR_RED     0xF800
#define TOPWAY_COLOR_GREEN   0x07E0
#define TOPWAY_COLOR_BLUE    0x001F
#define TOPWAY_COLOR_YELLOW  0xFFE0
#define TOPWAY_COLOR_CYAN    0x07FF
#define TOPWAY_COLOR_MAGENTA 0xF81F
#define TOPWAY_COLOR_ORANGE  0xFD20
#define TOPWAY_COLOR_GRAY    0x8410
#define TOPWAY_COLOR_DKGRAY  0x4208

#define VP_STR_BASE      0x000000
#define VP_N32_BASE      0x020000
#define VP_N64_BASE      0x030000
#define VP_BP1_BASE      0x040000
#define VP_G16_BASE      0x060000
#define VP_N16_BASE      0x080000

#define VP_STR_STATUS    0x000380  /* HOME: central device status text */
#define VP_STR_DIAG      0x000500  /* HOME: compact diagnostic message */
#define VP_STR_RTC_DATETIME 0x000800 /* ESP RTC date/time, local display string */
#define VP_N16_STATUS_COLOR 0x080006 /* HOME: device status text color, RGB565 */

/* WiFi configuration VP addresses */
#define VP_STR_WIFI_SSID         0x000900  /* WiFi SSID (string) - INPUT ONLY */
#define VP_STR_WIFI_PASSWORD     0x000080  /* WiFi Password (string) - INPUT ONLY */
#define VP_N16_WIFI_CONNECT_BTN  0x080020  /* WiFi Connect button - triggers reconnect */
#define VP_STR_WIFI_STATUS_MSG   0x000400  /* WiFi status message (string) - OUTPUT */

/* TELEGRAM page VP addresses */
#define VP_STR_TELEGRAM_QR_URL       0x000280  /* Telegram bot URL for QR widget */
#define VP_STR_TELEGRAM_TECH_INPUT   0x000000  /* 0x000000-BUFF: Technician number input */
#define VP_STR_TELEGRAM_STATUS_MSG   0x000C80  /* Authorization/status message */
#define VP_STR_TELEGRAM_AUTH_ROW0    0x000D00  /* Authorized technician row 0 */
#define VP_STR_TELEGRAM_AUTH_ROW1    0x000D80  /* Authorized technician row 1 */
#define VP_STR_TELEGRAM_AUTH_ROW2    0x000E00  /* Authorized technician row 2 */
#define VP_STR_TELEGRAM_AUTH_ROW3    0x000E80  /* Authorized technician row 3 */
#define VP_STR_TELEGRAM_AUTH_ROW4    0x000F00  /* Authorized technician row 4 */
#define VP_N16_TELEGRAM_AUTHORIZE_BTN 0x080060 /* Authorize button - set to 1 when pressed */
#define VP_N16_TELEGRAM_DELETE_ROW0   0x080026 /* Delete technician row 0 */
#define VP_N16_TELEGRAM_DELETE_ROW1   0x080028 /* Delete technician row 1 */
#define VP_N16_TELEGRAM_DELETE_ROW2   0x08002A /* Delete technician row 2 */
#define VP_N16_TELEGRAM_DELETE_ROW3   0x08002C /* Delete technician row 3 */
#define VP_N16_TELEGRAM_DELETE_ROW4   0x08002E /* Delete technician row 4 */

#define VP_N16_CUR_VALUE    0x080000  /* Current value, A */
#define VP_N16_TIN_VALUE    0x080002  /* Temp In, C */
#define VP_N16_TOUT_VALUE   0x080004  /* Temp Out, C */

/* CONFIGURATION page VP addresses (pure Celsius and Amperes) */
#define VP_N16_CFG_MIN_TIN      0x080030  /* Min Temp IN threshold (C) */
#define VP_N16_CFG_MIN_TOUT     0x080032  /* Min Temp OUT threshold (C) */
#define VP_N16_CFG_MIN_CURRENT  0x080034  /* Min Current threshold (A) */
#define VP_N16_CFG_MAX_TIN      0x080036  /* Max Temp IN threshold (C) */
#define VP_N16_CFG_MAX_TOUT     0x080038  /* Max Temp OUT threshold (C) */
#define VP_N16_CFG_MAX_CURRENT  0x08003A  /* Max Current threshold (A) */
#define VP_N16_CFG_DT_ALERT     0x080024  /* Delta temperature threshold (C) */
#define VP_N16_CFG_APPLY_BTN    0x08003C  /* Apply button - set to 1 when pressed */
#define VP_STR_CFG_STATUS_MSG   0x000600  /* Validation message under Apply */

/* SETTINGS page calibration VP addresses (absolute values, no x10 multiplier) */
#define VP_N16_CAL_CURRENT_SCALE     0x080050 /* Current scale in A/V */
#define VP_N16_CAL_TIN_OFFSET        0x080052 /* Temp In offset in C, signed */
#define VP_N16_CAL_TOUT_OFFSET       0x080054 /* Temp Out offset in C, signed */
#define VP_N16_CAL_ZERO_BTN          0x080056 /* Current zero button */
#define VP_N16_CAL_APPLY_BTN         0x080058 /* Apply calibration values */
#define VP_N16_CAL_SAVE_BTN          0x08005A /* Save calibration values */
#define VP_STR_CAL_STATUS_MSG        0x000700 /* Calibration status message */

#define WIFI_STATUS_OFFLINE   0
#define WIFI_STATUS_CONNECTED 1

#define DEVICE_STATE_ACTIVE   0
#define DEVICE_STATE_INACTIVE 1
#define DEVICE_STATE_WARNING  2
#define DEVICE_STATE_ERROR    3

#define TOPWAY_SYS_TIMER_CTRL0  0xFFFF00

typedef struct {
    uart_port_t uart_port;
    int tx_pin;
    int rx_pin;
    int rts_pin;
    uint32_t baud_rate;
} topway_config_t;

esp_err_t topway_init(const topway_config_t *config);
esp_err_t topway_deinit(void);

esp_err_t topway_handshake(void);
esp_err_t topway_read_version(char *out, size_t out_sz);
esp_err_t topway_read_page_id(uint16_t *page_id);

esp_err_t topway_set_sys_config(uint8_t baud_code, uint8_t touch_cfg);
esp_err_t topway_select_project(uint8_t prj_id);
esp_err_t topway_set_backlight(uint8_t level);
esp_err_t topway_screen_saver(uint16_t timeout_s, uint8_t dim_level);
esp_err_t topway_buzzer_ctrl(uint8_t loops, uint8_t t1, uint8_t t2, uint8_t freq1, uint8_t freq2);
esp_err_t topway_usb_unlock(const char *password);

esp_err_t topway_disp_page(uint16_t page_id);
esp_err_t topway_set_element_fg(uint8_t element, uint16_t page_id, uint8_t element_id, uint16_t color);
esp_err_t topway_set_element_bg(uint8_t element, uint16_t page_id, uint8_t element_id, uint8_t mode, uint16_t color);
esp_err_t topway_set_codepage(uint8_t country, uint8_t codepage);
esp_err_t topway_suspend_refresh(bool suspend);

esp_err_t topway_n16_write(uint32_t addr, uint16_t value);
esp_err_t topway_n16_read(uint32_t addr, uint16_t *value);
esp_err_t topway_n16_fill(uint32_t addr, uint16_t length, uint16_t value);
esp_err_t topway_n32_write(uint32_t addr, uint32_t value);
esp_err_t topway_n64_write(uint32_t addr, uint64_t value);
esp_err_t topway_str_write(uint32_t addr, const char *str);
esp_err_t topway_str_read(uint32_t addr, char *out, size_t out_sz);
esp_err_t topway_str_fill(uint32_t addr, uint16_t length, const char *str);
esp_err_t topway_successive_write(uint32_t addr, uint8_t length, const uint8_t *data, size_t data_len);
esp_err_t topway_g16_write(uint32_t addr, uint16_t size, const uint16_t *values);
esp_err_t topway_g16_write_rotate(uint32_t addr, uint16_t size, uint16_t value);

esp_err_t topway_sys_reg_write(uint32_t addr, uint8_t value);

esp_err_t topway_rtc_set(uint8_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t min, uint8_t sec);

/* Touch event callback */
typedef void (*topway_touch_callback_t)(uint8_t page_id, uint8_t key_id);

void topway_register_touch_callback(topway_touch_callback_t callback);
void topway_process_touch_events(void);

#ifdef __cplusplus
}
#endif

#endif
