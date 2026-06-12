#include "../../hal/display_hal.h"
#include "board.h"
#include "esp_lcd_sh8601.h"

#include <Arduino.h>
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define LCD_HOST SPI2_HOST
#define LCD_COL_OFFSET 20
#define TX_BUFFER_LINES 40

static esp_lcd_panel_io_handle_t panel_io = nullptr;
static esp_lcd_panel_handle_t panel = nullptr;

static SemaphoreHandle_t transfer_done = nullptr;
static uint16_t* tx_buffer = nullptr;

static constexpr size_t TX_BUFFER_PIXELS =
    static_cast<size_t>(LCD_WIDTH) * TX_BUFFER_LINES;

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

static inline uint16_t swap_rgb565_bytes(uint16_t color) {
    return static_cast<uint16_t>((color >> 8) | (color << 8));
}

static bool IRAM_ATTR color_transfer_done_callback(
    esp_lcd_panel_io_handle_t panel_io_handle,
    esp_lcd_panel_io_event_data_t* event_data,
    void* user_context
) {
    (void)panel_io_handle;
    (void)event_data;
    (void)user_context;

    BaseType_t task_woken = pdFALSE;

    if (transfer_done) {
        xSemaphoreGiveFromISR(transfer_done, &task_woken);
    }

    return task_woken == pdTRUE;
}

static bool send_buffer_blocking(
    int32_t x,
    int32_t y,
    int32_t w,
    int32_t h,
    const uint16_t* buffer
) {
    if (!panel || !buffer || !transfer_done) {
        return false;
    }

    // Eventuell übrig gebliebenes Signal entfernen.
    xSemaphoreTake(transfer_done, 0);

    esp_err_t err = esp_lcd_panel_draw_bitmap(
        panel,
        x + LCD_COL_OFFSET,
        y,
        x + LCD_COL_OFFSET + w,
        y + h,
        buffer
    );

    if (err != ESP_OK) {
        Serial.printf(
            "Display transfer failed: %s\n",
            esp_err_to_name(err)
        );
        return false;
    }

    if (xSemaphoreTake(
            transfer_done,
            pdMS_TO_TICKS(1000)
        ) != pdTRUE) {
        Serial.println("Display transfer timeout");
        return false;
    }

    return true;
}

void display_hal_init(void) {
    transfer_done = xSemaphoreCreateBinary();

    if (!transfer_done) {
        Serial.println("Display semaphore allocation failed");
        return;
    }

    tx_buffer = static_cast<uint16_t*>(
        heap_caps_malloc(
            TX_BUFFER_PIXELS * sizeof(uint16_t),
            MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL
        )
    );

    if (!tx_buffer) {
        Serial.println("Display DMA buffer allocation failed");
        return;
    }

    spi_bus_config_t bus_config =
        SH8601_PANEL_BUS_QSPI_CONFIG(
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
        Serial.printf(
            "SPI init failed: %s\n",
            esp_err_to_name(err)
        );
        return;
    }

    esp_lcd_panel_io_spi_config_t io_config =
        SH8601_PANEL_IO_QSPI_CONFIG(
            LCD_CS,
            color_transfer_done_callback,
            nullptr
        );

    // Wir warten auf jeden Transfer, daher genügt eine Transaktion.
    io_config.trans_queue_depth = 1;

    err = esp_lcd_new_panel_io_spi(
        static_cast<esp_lcd_spi_bus_handle_t>(LCD_HOST),
        &io_config,
        &panel_io
    );

    if (err != ESP_OK) {
        Serial.printf(
            "Panel IO init failed: %s\n",
            esp_err_to_name(err)
        );
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
        Serial.printf(
            "SH8601 init failed: %s\n",
            esp_err_to_name(err)
        );
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
    if (!panel || !tx_buffer) {
        return;
    }

    const uint16_t swapped = swap_rgb565_bytes(color);

    for (size_t i = 0; i < TX_BUFFER_PIXELS; i++) {
        tx_buffer[i] = swapped;
    }

    for (int32_t y = 0; y < LCD_HEIGHT; y += TX_BUFFER_LINES) {
        int32_t rows = LCD_HEIGHT - y;

        if (rows > TX_BUFFER_LINES) {
            rows = TX_BUFFER_LINES;
        }

        if (!send_buffer_blocking(
                0,
                y,
                LCD_WIDTH,
                rows,
                tx_buffer
            )) {
            return;
        }
    }
}

void display_hal_draw_bitmap(
    int32_t x,
    int32_t y,
    int32_t w,
    int32_t h,
    const uint16_t* pixels
) {
    if (!panel || !pixels || !tx_buffer || w <= 0 || h <= 0) {
        return;
    }

    size_t rows_per_chunk = TX_BUFFER_PIXELS / static_cast<size_t>(w);

    if (rows_per_chunk == 0) {
        Serial.println("Display region is wider than TX buffer");
        return;
    }

    int32_t row = 0;

    while (row < h) {
        int32_t rows = h - row;

        if (static_cast<size_t>(rows) > rows_per_chunk) {
            rows = static_cast<int32_t>(rows_per_chunk);
        }

        const size_t pixel_count =
            static_cast<size_t>(w) * rows;

        const uint16_t* source =
            pixels + static_cast<size_t>(row) * w;

        // LVGL liefert RGB565 im Speicher Little-Endian.
        // Das Panel erwartet für jedes Pixel das höherwertige Byte zuerst.
        for (size_t i = 0; i < pixel_count; i++) {
            tx_buffer[i] = swap_rgb565_bytes(source[i]);
        }

        if (!send_buffer_blocking(
                x,
                y + row,
                w,
                rows,
                tx_buffer
            )) {
            return;
        }

        row += rows;
    }
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