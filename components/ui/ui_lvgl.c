#include "sdkconfig.h"

#ifndef CONFIG_IDMS_LCD_MIRROR_X
#define CONFIG_IDMS_LCD_MIRROR_X 0
#endif
#ifndef CONFIG_IDMS_LCD_MIRROR_Y
#define CONFIG_IDMS_LCD_MIRROR_Y 0
#endif

#if CONFIG_IDMS_UI_ENABLE

#include "lvgl.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#if CONFIG_IDMS_DISPLAY_ILI9341
#include "esp_lcd_ili9341.h"
#elif CONFIG_IDMS_DISPLAY_ST7796S
#include "esp_lcd_st7796s.h"
#elif CONFIG_IDMS_DISPLAY_ILI9488
#include "esp_lcd_ili9488.h"
#endif
#if CONFIG_IDMS_LCD_BUS_I80
#include "esp_lcd_panel_io_i80.h"
#endif
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "monitor.h"
#include "config_store.h"
#include "pins.h"
#include "xpt2046.h"
#include "ota.h"

#include <stdio.h>
#include <string.h>
#include <assert.h>

static const char *TAG = "ui";

#if CONFIG_IDMS_DISPLAY_ST7796S
#define LCD_H_RES 480
#define LCD_V_RES 320
#if CONFIG_IDMS_LCD_BUS_SPI
#define LCD_PIXEL_CLOCK_HZ (10 * 1000 * 1000)
#elif CONFIG_IDMS_LCD_BUS_I80
#define LCD_PIXEL_CLOCK_HZ (CONFIG_IDMS_PIN_LCD_I80_PCLK_HZ)
#endif
#define LV_BUF_LINES 50
#elif CONFIG_IDMS_DISPLAY_ILI9488
#define LCD_H_RES 480
#define LCD_V_RES 320
#if CONFIG_IDMS_LCD_BUS_SPI
#define LCD_PIXEL_CLOCK_HZ (20 * 1000 * 1000)
#elif CONFIG_IDMS_LCD_BUS_I80
#define LCD_PIXEL_CLOCK_HZ (CONFIG_IDMS_PIN_LCD_I80_PCLK_HZ)
#endif
#define LV_BUF_LINES 50
#elif CONFIG_IDMS_DISPLAY_ILI9341
#define LCD_H_RES 320
#define LCD_V_RES 240
#define LCD_PIXEL_CLOCK_HZ (20 * 1000 * 1000)
#define LV_BUF_LINES 40
#endif

#define LCD_CMD_BITS 8
#define LCD_PARAM_BITS 8
#define LVGL_TICK_MS 2
#define LVGL_TASK_STACK (8 * 1024)
#define LVGL_TASK_PRIO 4

static SemaphoreHandle_t s_lvgl_mux;
static lv_disp_draw_buf_t s_disp_buf;
static lv_disp_drv_t s_disp_drv;
static lv_indev_drv_t s_indev_drv;
static lv_disp_t *s_disp;

static lv_obj_t *s_lbl_i;
static lv_obj_t *s_lbl_tin;
static lv_obj_t *s_lbl_tout;
static lv_obj_t *s_lbl_dt;
static lv_obj_t *s_lbl_wifi;
static lv_obj_t *s_lbl_version;
static lv_obj_t *s_lbl_ota_status;
static lv_obj_t *s_lbl_tech_count;

static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;

#if CONFIG_IDMS_LCD_BUS_I80
static esp_lcd_i80_bus_handle_t s_i80_bus;
#endif

static bool on_flush_ready(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    (void)panel_io;
    (void)edata;
    lv_disp_drv_t *disp_drv = (lv_disp_drv_t *)user_ctx;
    lv_disp_flush_ready(disp_drv);
    return false;
}

static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
#if CONFIG_IDMS_DISPLAY_ILI9341
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)drv->user_data;
    int x1 = area->x1;
    int y1 = area->y1;
    int x2 = area->x2;
    int y2 = area->y2;
    esp_lcd_panel_draw_bitmap(panel, x1, y1, x2 + 1, y2 + 1, color_map);
#elif CONFIG_IDMS_DISPLAY_ST7796S || CONFIG_IDMS_DISPLAY_ILI9488
    int x1 = area->x1;
    int y1 = area->y1;
    int x2 = area->x2 + 1;
    int y2 = area->y2 + 1;
    uint8_t col[4] = { (x1 >> 8) & 0xFF, x1 & 0xFF, ((x2 - 1) >> 8) & 0xFF, (x2 - 1) & 0xFF };
    uint8_t row[4] = { (y1 >> 8) & 0xFF, y1 & 0xFF, ((y2 - 1) >> 8) & 0xFF, (y2 - 1) & 0xFF };
    esp_lcd_panel_io_tx_param(s_io, 0x2A, col, 4);
    esp_lcd_panel_io_tx_param(s_io, 0x2B, row, 4);
    size_t len = (x2 - x1) * (y2 - y1) * sizeof(lv_color_t);
    esp_lcd_panel_io_tx_color(s_io, 0x2C, color_map, len);
#endif
}

#if CONFIG_IDMS_PIN_TOUCH_CS >= 0
static void touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    (void)drv;
    int16_t x = 0, y = 0;
    bool pressed = false;
    if (!xpt2046_read(&x, &y, &pressed)) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    if (!pressed) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    int sx = ((int)x * LCD_H_RES) / 4096;
    int sy = ((int)y * LCD_V_RES) / 4096;
    if (sx < 0) sx = 0;
    if (sy < 0) sy = 0;
    if (sx >= LCD_H_RES) sx = LCD_H_RES - 1;
    if (sy >= LCD_V_RES) sy = LCD_V_RES - 1;

    data->point.x = sx;
    data->point.y = sy;
    data->state = LV_INDEV_STATE_PRESSED;
}
#endif

static void lv_tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(LVGL_TICK_MS);
}

static bool lvgl_lock(int timeout_ms)
{
    const TickType_t t = (timeout_ms < 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTakeRecursive(s_lvgl_mux, t) == pdTRUE;
}

static void lvgl_unlock(void)
{
    xSemaphoreGiveRecursive(s_lvgl_mux);
}

static void lvgl_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t delay = 30;
        if (lvgl_lock(-1)) {
            delay = lv_timer_handler();
            lvgl_unlock();
        }
        if (delay > 500) {
            delay = 500;
        }
        vTaskDelay(pdMS_TO_TICKS(delay));
    }
}

static void on_timer_refresh(lv_timer_t *t)
{
    (void)t;
    idms_metrics_t m;
    monitor_get_metrics(&m);

    char buf[96];
    if (m.current_valid) {
        snprintf(buf, sizeof(buf), "I = %.2f A", m.current_a);
    } else {
        snprintf(buf, sizeof(buf), "I: --");
    }
    lv_label_set_text(s_lbl_i, buf);

    if (m.t_in_valid) {
        snprintf(buf, sizeof(buf), "Tin: %.1f C", m.t_in_c);
    } else {
        snprintf(buf, sizeof(buf), "Tin: --");
    }
    lv_label_set_text(s_lbl_tin, buf);

    if (m.t_out_valid) {
        snprintf(buf, sizeof(buf), "Tout: %.1f C", m.t_out_c);
    } else {
        snprintf(buf, sizeof(buf), "Tout: --");
    }
    lv_label_set_text(s_lbl_tout, buf);

    if (m.delta_valid) {
        snprintf(buf, sizeof(buf), "dT: %.1f C", m.delta_t_c);
    } else {
        snprintf(buf, sizeof(buf), "dT: --");
    }
    lv_label_set_text(s_lbl_dt, buf);

    if (m.wifi_connected) {
        snprintf(buf, sizeof(buf), "WiFi: %s", m.wifi_ip[0] ? m.wifi_ip : "OK");
    } else {
        snprintf(buf, sizeof(buf), "WiFi: offline");
    }
    lv_label_set_text(s_lbl_wifi, buf);

    lv_label_set_text(s_lbl_version, ota_get_version());

    char ota_buf[64];
    snprintf(ota_buf, sizeof(ota_buf), "OTA: %s (%s)", ota_get_status(), ota_get_partition());
    lv_label_set_text(s_lbl_ota_status, ota_buf);

    snprintf(buf, sizeof(buf), "Techs: %u", (unsigned)config_get_tech_count());
    lv_label_set_text(s_lbl_tech_count, buf);
}

static lv_obj_t *s_ta_tech;

static void on_save_tech(lv_event_t *e)
{
    (void)e;
    const char *txt = lv_textarea_get_text(s_ta_tech);
    if (!txt || txt[0] == '\0') {
        return;
    }
    esp_err_t err = config_add_tech_id(txt);
    if (err == ESP_OK) {
        lv_textarea_set_text(s_ta_tech, "");
        ESP_LOGI(TAG, "Saved technician id");
    } else {
        ESP_LOGW(TAG, "Save failed: %s", esp_err_to_name(err));
    }
}

static void build_ui(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "ESP-IDMS");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);

    s_lbl_i = lv_label_create(scr);
    s_lbl_tin = lv_label_create(scr);
    s_lbl_tout = lv_label_create(scr);
    s_lbl_dt = lv_label_create(scr);
    s_lbl_wifi = lv_label_create(scr);
    s_lbl_version = lv_label_create(scr);
    s_lbl_ota_status = lv_label_create(scr);
    s_lbl_tech_count = lv_label_create(scr);

    lv_obj_align(s_lbl_i, LV_ALIGN_TOP_LEFT, 8, 28);
    lv_obj_align(s_lbl_tin, LV_ALIGN_TOP_LEFT, 8, 50);
    lv_obj_align(s_lbl_tout, LV_ALIGN_TOP_LEFT, 8, 72);
    lv_obj_align(s_lbl_dt, LV_ALIGN_TOP_LEFT, 8, 94);
    lv_obj_align(s_lbl_wifi, LV_ALIGN_TOP_LEFT, 8, 120);
    lv_obj_align(s_lbl_version, LV_ALIGN_TOP_LEFT, 8, 146);
    lv_obj_align(s_lbl_ota_status, LV_ALIGN_TOP_LEFT, 8, 168);
    lv_obj_align(s_lbl_tech_count, LV_ALIGN_TOP_LEFT, 8, 192);

    const lv_font_t *value_font = &lv_font_montserrat_14;
    lv_obj_set_style_text_font(s_lbl_i, value_font, 0);
    lv_obj_set_style_text_font(s_lbl_tin, value_font, 0);
    lv_obj_set_style_text_font(s_lbl_tout, value_font, 0);
    lv_obj_set_style_text_font(s_lbl_dt, value_font, 0);
    lv_obj_set_style_text_font(s_lbl_wifi, value_font, 0);
    lv_obj_set_style_text_font(s_lbl_version, value_font, 0);
    lv_obj_set_style_text_font(s_lbl_ota_status, value_font, 0);
    lv_obj_set_style_text_font(s_lbl_tech_count, value_font, 0);

#if CONFIG_IDMS_PIN_TOUCH_CS >= 0
    lv_obj_t *lbl_add = lv_label_create(scr);
    lv_label_set_text(lbl_add, "Add Telegram chat_id:");
    lv_obj_align(lbl_add, LV_ALIGN_TOP_LEFT, 8, 220);

    s_ta_tech = lv_textarea_create(scr);
    lv_textarea_set_one_line(s_ta_tech, true);
    lv_obj_set_width(s_ta_tech, LCD_H_RES - 110);
    lv_obj_align(s_ta_tech, LV_ALIGN_TOP_LEFT, 8, 244);

    lv_obj_t *btn = lv_btn_create(scr);
    lv_obj_set_size(btn, 90, 30);
    lv_obj_align(btn, LV_ALIGN_TOP_RIGHT, -8, 241);
    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, "Save");
    lv_obj_center(bl);
    lv_obj_add_event_cb(btn, on_save_tech, LV_EVENT_CLICKED, NULL);
#endif

    lv_timer_create(on_timer_refresh, 500, NULL);
}

void idms_ui_init(void)
{
#if CONFIG_IDMS_DISPLAY_ST7796S
#if CONFIG_IDMS_LCD_BUS_SPI
    ESP_LOGI(TAG, "UI init (LVGL + ST7796 480x320 SPI)");
#elif CONFIG_IDMS_LCD_BUS_I80
    ESP_LOGI(TAG, "UI init (LVGL + ST7796 480x320 I80)");
#endif
#elif CONFIG_IDMS_DISPLAY_ILI9488
#if CONFIG_IDMS_LCD_BUS_SPI
    ESP_LOGI(TAG, "UI init (LVGL + ILI9488 480x320 SPI)");
#elif CONFIG_IDMS_LCD_BUS_I80
    ESP_LOGI(TAG, "UI init (LVGL + ILI9488 480x320 I80)");
#endif
#elif CONFIG_IDMS_DISPLAY_ILI9341
    ESP_LOGI(TAG, "UI init (LVGL + ILI9341 320x240 SPI)");
#endif

    if (CONFIG_IDMS_PIN_LCD_BL >= 0) {
        gpio_config_t io = {
            .pin_bit_mask = 1ULL << CONFIG_IDMS_PIN_LCD_BL,
            .mode = GPIO_MODE_OUTPUT,
        };
        gpio_config(&io);
        gpio_set_level(CONFIG_IDMS_PIN_LCD_BL, 1);
    }

    lv_init();

    lv_color_t *buf1 = heap_caps_malloc(LCD_H_RES * LV_BUF_LINES * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!buf1) {
        ESP_LOGW(TAG, "LVGL buf1: DMA alloc failed, trying SPIRAM");
        buf1 = heap_caps_malloc(LCD_H_RES * LV_BUF_LINES * sizeof(lv_color_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    lv_color_t *buf2 = heap_caps_malloc(LCD_H_RES * LV_BUF_LINES * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!buf2) {
        ESP_LOGW(TAG, "LVGL buf2: DMA alloc failed, trying SPIRAM");
        buf2 = heap_caps_malloc(LCD_H_RES * LV_BUF_LINES * sizeof(lv_color_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    assert(buf1 && buf2);
    ESP_LOGI(TAG, "LVGL buf1=%p buf2=%p", buf1, buf2);
    lv_disp_draw_buf_init(&s_disp_buf, buf1, buf2, LCD_H_RES * LV_BUF_LINES);

    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res = LCD_H_RES;
    s_disp_drv.ver_res = LCD_V_RES;
    s_disp_drv.flush_cb = flush_cb;
    s_disp_drv.draw_buf = &s_disp_buf;
    s_disp_drv.user_data = NULL;

    /* =====================================================================
     * Create panel IO — SPI or I80 depending on menuconfig
     * ===================================================================== */
#if CONFIG_IDMS_LCD_BUS_SPI
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = CONFIG_IDMS_PIN_LCD_DC,
        .cs_gpio_num = CONFIG_IDMS_PIN_LCD_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = LCD_CMD_BITS,
        .lcd_param_bits = LCD_PARAM_BITS,
        .spi_mode = 0,
        .trans_queue_depth = 10,
        .on_color_trans_done = on_flush_ready,
        .user_ctx = &s_disp_drv,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)IDMS_SPI_HOST, &io_cfg, &s_io));

#elif CONFIG_IDMS_LCD_BUS_I80
    ESP_LOGI(TAG, "Creating I80 bus (D0=%d..D7=%d, WR=%d, DC=%d, CS=%d)",
             CONFIG_IDMS_PIN_LCD_I80_D0, CONFIG_IDMS_PIN_LCD_I80_D7,
             CONFIG_IDMS_PIN_LCD_I80_WR, CONFIG_IDMS_PIN_LCD_DC, CONFIG_IDMS_PIN_LCD_CS);

    if (CONFIG_IDMS_PIN_LCD_I80_RD >= 0) {
        gpio_config_t rd_io = {
            .pin_bit_mask = 1ULL << CONFIG_IDMS_PIN_LCD_I80_RD,
            .mode = GPIO_MODE_OUTPUT,
        };
        gpio_config(&rd_io);
        gpio_set_level(CONFIG_IDMS_PIN_LCD_I80_RD, 1);
    }

    esp_lcd_i80_bus_config_t bus_cfg = {
        .dc_gpio_num = CONFIG_IDMS_PIN_LCD_DC,
        .wr_gpio_num = CONFIG_IDMS_PIN_LCD_I80_WR,
        .data_gpio_nums = {
            CONFIG_IDMS_PIN_LCD_I80_D0,
            CONFIG_IDMS_PIN_LCD_I80_D1,
            CONFIG_IDMS_PIN_LCD_I80_D2,
            CONFIG_IDMS_PIN_LCD_I80_D3,
            CONFIG_IDMS_PIN_LCD_I80_D4,
            CONFIG_IDMS_PIN_LCD_I80_D5,
            CONFIG_IDMS_PIN_LCD_I80_D6,
            CONFIG_IDMS_PIN_LCD_I80_D7,
        },
        .bus_width = 8,
        .max_transfer_bytes = LCD_H_RES * LV_BUF_LINES * sizeof(lv_color_t),
        .psram_trans_align = 64,
        .sram_trans_align = 4,
    };
    ESP_ERROR_CHECK(esp_lcd_new_i80_bus(&bus_cfg, &s_i80_bus));

    esp_lcd_panel_io_i80_config_t io_cfg = {
        .cs_gpio_num = CONFIG_IDMS_PIN_LCD_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = LCD_CMD_BITS,
        .lcd_param_bits = LCD_PARAM_BITS,
        .on_color_trans_done = on_flush_ready,
        .user_ctx = &s_disp_drv,
        .flags = {
            .cs_active_high = 0,
        },
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i80(s_i80_bus, &io_cfg, &s_io));
    ESP_LOGI(TAG, "I80 panel IO created (pclk=%d Hz)", LCD_PIXEL_CLOCK_HZ);
#endif

    /* =====================================================================
     * Controller-specific initialization
     * ===================================================================== */
#if CONFIG_IDMS_DISPLAY_ILI9341
    esp_lcd_panel_dev_config_t panel_cfg = {
#if CONFIG_IDMS_PIN_LCD_RST >= 0
        .reset_gpio_num = CONFIG_IDMS_PIN_LCD_RST,
#else
        .reset_gpio_num = -1,
#endif
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(s_io, &panel_cfg, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

#if CONFIG_IDMS_LCD_SWAP_XY
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel, true));
#else
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel, false));
#endif
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, CONFIG_IDMS_LCD_MIRROR_X, CONFIG_IDMS_LCD_MIRROR_Y));

#elif CONFIG_IDMS_DISPLAY_ST7796S || CONFIG_IDMS_DISPLAY_ILI9488
    /* ST7796/ILI9488: init via direct commands (no vtable panel needed) */
    if (CONFIG_IDMS_PIN_LCD_RST >= 0) {
        gpio_config_t rst_io = {
            .pin_bit_mask = 1ULL << CONFIG_IDMS_PIN_LCD_RST,
            .mode = GPIO_MODE_OUTPUT,
        };
        gpio_config(&rst_io);
        gpio_set_level(CONFIG_IDMS_PIN_LCD_RST, 0);
        vTaskDelay(pdMS_TO_TICKS(20));
        gpio_set_level(CONFIG_IDMS_PIN_LCD_RST, 1);
        vTaskDelay(pdMS_TO_TICKS(120));
    }

#if CONFIG_IDMS_DISPLAY_ST7796S
    ESP_ERROR_CHECK(st7796s_lcd_init(s_io));
#elif CONFIG_IDMS_DISPLAY_ILI9488
    ESP_ERROR_CHECK(ili9488_lcd_init(s_io));
#endif

#if CONFIG_IDMS_LCD_SWAP_XY
    uint8_t madctl_swap = 0x00;
    madctl_swap |= (1 << 3); /* BGR */
    madctl_swap |= (1 << 5); /* MV: row/column exchange (landscape) */
    if (CONFIG_IDMS_LCD_MIRROR_X) madctl_swap |= (1 << 6); /* MX */
    if (CONFIG_IDMS_LCD_MIRROR_Y) madctl_swap |= (1 << 7); /* MY */
    esp_lcd_panel_io_tx_param(s_io, 0x36, &madctl_swap, 1);
    uint8_t col_landscape[4] = {0x00, 0x00, 0x01, 0xDF}; /* 0..479 */
    uint8_t row_landscape[4] = {0x00, 0x00, 0x01, 0x3F}; /* 0..319 */
    esp_lcd_panel_io_tx_param(s_io, 0x2A, col_landscape, 4);
    esp_lcd_panel_io_tx_param(s_io, 0x2B, row_landscape, 4);
#if CONFIG_IDMS_DISPLAY_ST7796S
    ESP_LOGI(TAG, "ST7796 landscape: MADCTL=0x%02X", madctl_swap);
#elif CONFIG_IDMS_DISPLAY_ILI9488
    ESP_LOGI(TAG, "ILI9488 landscape: MADCTL=0x%02X", madctl_swap);
#endif
#else
    if (CONFIG_IDMS_LCD_MIRROR_X || CONFIG_IDMS_LCD_MIRROR_Y) {
        uint8_t madctl_mirror = 0x00;
        madctl_mirror |= (1 << 3); /* BGR */
        if (CONFIG_IDMS_LCD_MIRROR_X) madctl_mirror |= (1 << 6); /* MX */
        if (CONFIG_IDMS_LCD_MIRROR_Y) madctl_mirror |= (1 << 7); /* MY */
        esp_lcd_panel_io_tx_param(s_io, 0x36, &madctl_mirror, 1);
    }
#endif
    s_panel = NULL;

#if CONFIG_IDMS_DISPLAY_ST7796S
#if CONFIG_IDMS_LCD_BUS_SPI
    st7796s_lcd_test_read_id(s_io);
#endif
    st7796s_lcd_fill_color(s_io, 0xF800, 320, 480);
    vTaskDelay(pdMS_TO_TICKS(3000));
    st7796s_lcd_fill_color(s_io, 0x07E0, 320, 480);
    vTaskDelay(pdMS_TO_TICKS(3000));
    st7796s_lcd_fill_color(s_io, 0x001F, 320, 480);
    vTaskDelay(pdMS_TO_TICKS(3000));
    st7796s_lcd_fill_color(s_io, 0x0000, 320, 480);
    vTaskDelay(pdMS_TO_TICKS(500));
#elif CONFIG_IDMS_DISPLAY_ILI9488
    ili9488_lcd_fill_color(s_io, 0xF800, LCD_H_RES, LCD_V_RES);
    vTaskDelay(pdMS_TO_TICKS(2000));
    ili9488_lcd_fill_color(s_io, 0x07E0, LCD_H_RES, LCD_V_RES);
    vTaskDelay(pdMS_TO_TICKS(2000));
    ili9488_lcd_fill_color(s_io, 0x001F, LCD_H_RES, LCD_V_RES);
    vTaskDelay(pdMS_TO_TICKS(2000));
    ili9488_lcd_fill_color(s_io, 0x0000, LCD_H_RES, LCD_V_RES);
    vTaskDelay(pdMS_TO_TICKS(500));
#endif
#endif

#if CONFIG_IDMS_PIN_TOUCH_CS >= 0
    ESP_ERROR_CHECK(xpt2046_init(IDMS_SPI_HOST));
#endif

#if CONFIG_IDMS_DISPLAY_ILI9341
    s_disp_drv.user_data = s_panel;
#endif
    s_disp = lv_disp_drv_register(&s_disp_drv);

#if CONFIG_IDMS_PIN_TOUCH_CS >= 0
    lv_indev_drv_init(&s_indev_drv);
    s_indev_drv.type = LV_INDEV_TYPE_POINTER;
    s_indev_drv.disp = s_disp;
    s_indev_drv.read_cb = touch_read_cb;
    lv_indev_drv_register(&s_indev_drv);
#endif

    const esp_timer_create_args_t tick_args = {
        .callback = &lv_tick_cb,
        .name = "lv_tick",
    };
    esp_timer_handle_t tick = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &tick));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick, LVGL_TICK_MS * 1000));

    s_lvgl_mux = xSemaphoreCreateRecursiveMutex();
    assert(s_lvgl_mux);

    xTaskCreatePinnedToCore(lvgl_task, "lvgl", LVGL_TASK_STACK, NULL, LVGL_TASK_PRIO, NULL, 1);

    vTaskDelay(pdMS_TO_TICKS(50));
    if (lvgl_lock(-1)) {
        build_ui();
        lvgl_unlock();
    }

    ESP_LOGI(TAG, "UI ready (%dx%d)", LCD_H_RES, LCD_V_RES);
}

#else

#include "esp_log.h"
static const char *TAG = "ui";

void idms_ui_init(void)
{
    ESP_LOGI(TAG, "UI disabled (menuconfig -> ESP-IDMS Configuration -> UI)");
}

#endif