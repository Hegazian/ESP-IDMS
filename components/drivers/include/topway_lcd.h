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

#define VP_STR_CURRENT   0x000000
#define VP_STR_TIN       0x000080
#define VP_STR_TOUT      0x000100
#define VP_STR_DT        0x000180
#define VP_STR_WIFI      0x000200
#define VP_STR_VERSION   0x000280
#define VP_STR_OTA       0x000300

#define VP_N16_CUR_X10      0x080000
#define VP_N16_TIN_X10      0x080002
#define VP_N16_TOUT_X10     0x080004
#define VP_N16_DT_X10       0x080006

/* Valid flags and extra status variables - placed after the 4 main display vars */
#define VP_N16_CUR_VALID    0x080010
#define VP_N16_TIN_VALID    0x080012
#define VP_N16_TOUT_VALID   0x080014
#define VP_N16_DT_VALID     0x080016
#define VP_N16_WIFI_STATUS  0x080018
#define VP_N16_OTA_STATUS   0x08001A
#define VP_N16_TECH_COUNT   0x08001C
#define VP_N16_POWER_FAULT  0x08001E
#define VP_N16_COOL_FAULT   0x080020

#define WIFI_STATUS_OFFLINE   0
#define WIFI_STATUS_CONNECTED 1

#define OTA_STATUS_READY    0
#define OTA_STATUS_UPDATING 1
#define OTA_STATUS_ERROR    2

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

esp_err_t topway_disp_page(uint16_t page_id);
esp_err_t topway_set_element_fg(uint8_t element, uint16_t page_id, uint8_t element_id, uint16_t color);
esp_err_t topway_set_element_bg(uint8_t element, uint16_t page_id, uint8_t element_id, uint8_t mode, uint16_t color);
esp_err_t topway_set_codepage(uint8_t country, uint8_t codepage);
esp_err_t topway_suspend_refresh(bool suspend);

esp_err_t topway_n16_write(uint32_t addr, uint16_t value);
esp_err_t topway_n16_fill(uint32_t addr, uint16_t length, uint16_t value);
esp_err_t topway_n32_write(uint32_t addr, uint32_t value);
esp_err_t topway_n64_write(uint32_t addr, uint64_t value);
esp_err_t topway_str_write(uint32_t addr, const char *str);
esp_err_t topway_str_fill(uint32_t addr, uint16_t length, const char *str);
esp_err_t topway_successive_write(uint32_t addr, uint8_t length, const uint8_t *data, size_t data_len);
esp_err_t topway_g16_write(uint32_t addr, uint16_t size, const uint16_t *values);
esp_err_t topway_g16_write_rotate(uint32_t addr, uint16_t size, uint16_t value);

esp_err_t topway_sys_reg_write(uint32_t addr, uint8_t value);

esp_err_t topway_rtc_set(uint8_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t min, uint8_t sec);

#ifdef __cplusplus
}
#endif

#endif
