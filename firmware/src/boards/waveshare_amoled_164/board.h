#pragma once

// Template board.h — copy this file (and the rest of boards/template/) to
// boards/<your_board>/ and fill in each TODO. See docs/porting/adding-a-board.md
// for a walk-through.

#define BOARD_NAME           "Waveshare ESP32-S3 Touch AMOLED 1.64"

// ---- Display geometry ----
// Active panel pixel dimensions (post-orientation).
#define LCD_WIDTH            280
#define LCD_HEIGHT           456

// ---- QSPI display pins ----
// Wire your panel datasheet's QSPI pins to MCU GPIOs and list them here.
#define LCD_CS               9
#define LCD_SCLK             10
#define LCD_SDIO0            11
#define LCD_SDIO1            12
#define LCD_SDIO2            13
#define LCD_SDIO3            14
#define LCD_RESET            21

// ---- I2C bus (shared by touch, PMU, IMU, IO expander) ----
#define IIC_SDA              47
#define IIC_SCL              48

// ---- Touch ----
// I2C address depends on the controller. Replace TODO_TP_ADDR below and pick
// the matching driver in touch.cpp.
#define TP_INT               -1
#define TP_ADDR              0x38

// ---- PMU ----
// Drop or change if your board doesn't ship an AXP2101.
#define AXP2101_ADDR         0x34

// ---- Buttons ----
#define BTN_BACK_GPIO        0     // BOOT — primary, Space (PTT)
// If your board has a second physical button, set its GPIO and bump
// BOARD_HAS_SECONDARY_BUTTON to 1 below.

// ---- Capability flags ----
// Compile-time switches the linker uses to dead-strip optional features.
// Keep these in sync with the BoardCaps instance in caps.cpp.
#define BOARD_HAS_SECONDARY_BUTTON 0
#define BOARD_HAS_ROTATION         0
#define BOARD_HAS_IMU              0
#define BOARD_HAS_BATTERY          0
#define BOARD_HAS_IO_EXPANDER      0
