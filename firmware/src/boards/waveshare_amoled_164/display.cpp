#include "../../hal/display_hal.h"
#include "board.h"
#include "esp_lcd_sh8601.h"

#include <Arduino.h>
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

#define LCD_HOST SPI2_HOST

static esp_lcd_panel_io_handle_t panel_io = nullptr;
static esp_lcd_panel_handle_t panel = nullptr;

static const uint8_t cmd_c4[] = {0x80};
static const uint8_t cmd_35[] = {0x00};
static const uint8_t cmd_53[] = {0x20};
static const uint8_t cmd_63[] = {0xFF};
static const uint8_t cmd_51_off[] = {0x00};
static const uint8_t cmd_51_on[] = {0xFF};

static const sh8601_lcd_init_cmd_t lcd_init_cmds[] = {
    {0x11, nullptr, 0, 80},
    {0xC4, cmd_c4, 1, 0},
    {0x35, cmd_35, 1, 0},
    {0x53, cmd_53, 1, 1},
    {0x63, cmd_63, 1, 1},
    {0x51, cmd_51_off, 1, 1},
    {0x29, nullptr, 0, 10},
    {0x51, cmd_51_on, 1, 0},
};

void display_hal_init(void) {
    spi_bus_config_t bus_config = SH8601_PANEL_BUS_QSPI_CONFIG(
        LCD_SCLK,
        LCD_SDIO0,
        LCD_SDIO1,
        LCD_SDIO2,
        LCD_SDIO3,
        LCD_WIDTH * LCD_HEIGHT * 2
    );

    esp_err_t err = spi_bus_initialize(
        LCD_HOST,
        &bus_config,
        SPI_DMA_CH_AUTO
    );

    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        Serial.printf("SPI init failed: %s\n", esp_err_to_name(err));
        return;
    }

    esp_lcd_panel_io_spi_config_t io_config =
        SH8601_PANEL_IO_QSPI_CONFIG(
            LCD_CS,
            nullptr,
            nullptr
        );

    err = esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)LCD_HOST,
        &io_config,
        &panel_io
    );

    if (err != ESP_OK) {
        Serial.printf("Panel IO init failed: %s\n", esp_err_to_name(err));
        return;
    }

    sh8601_vendor_config_t vendor_config = {};
    vendor_config.init_cmds = lcd_init_cmds;
    vendor_config.init_cmds_size =
        sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]);
    vendor_config.flags.use_qspi_interface = 1;

    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = LCD_RESET;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.bits_per_pixel = 16;
    panel_config.vendor_config = &vendor_config;

    err = esp_lcd_new_panel_sh8601(
        panel_io,
        &panel_config,
        &panel
    );

    if (err != ESP_OK) {
        Serial.printf("SH8601 init failed: %s\n", esp_err_to_name(err));
        panel = nullptr;
        return;
    }

    Serial.println("SH8601 driver created");
}

void display_hal_begin(void) {
    if (!panel) {
        Serial.println("Display panel not available");
        return;
    }

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    display_hal_set_brightness(255);
    display_hal_fill_screen(0x0000);

    Serial.println("SH8601 display started");
}

void display_hal_set_brightness(uint8_t level) {
    if (!panel_io) {
        return;
    }

    uint32_t command = 0x51;
    command &= 0xFF;
    command <<= 8;
    command |= 0x02UL << 24;

    esp_lcd_panel_io_tx_param(
        panel_io,
        command,
        &level,
        1
    );
}

void display_hal_fill_screen(uint16_t color) {
    static uint16_t line[LCD_WIDTH];

    for (int x = 0; x < LCD_WIDTH; x++) {
        line[x] = color;
    }

    for (int y = 0; y < LCD_HEIGHT; y++) {
        esp_lcd_panel_draw_bitmap(
            panel,
            20,
            y,
            20 + LCD_WIDTH,
            y + 1,
            line
        );
    }
}

void display_hal_draw_bitmap(
    int32_t x,
    int32_t y,
    int32_t w,
    int32_t h,
    const uint16_t* pixels
) {
    if (!panel || !pixels || w <= 0 || h <= 0) {
        return;
    }

    esp_lcd_panel_draw_bitmap(
        panel,
        x + 20,
        y,
        x + 20 + w,
        y + h,
        pixels
    );
}

void display_hal_tick(void) {
}

void display_hal_round_area(
    int32_t* x1,
    int32_t* y1,
    int32_t* x2,
    int32_t* y2
) {
    *x1 &= ~1;
    *y1 &= ~1;
    *x2 |= 1;
    *y2 |= 1;
}