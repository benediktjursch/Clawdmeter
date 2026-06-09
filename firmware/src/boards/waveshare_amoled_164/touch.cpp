#include "../../hal/touch_hal.h"
#include "board.h"
#include <Arduino.h>
#include <Wire.h>

// TODO: replace the body with a driver for your controller. Two patterns:
//   1. A library (SensorLib's CSTxxx, TAMC_GT911, etc.) — add to lib_deps
//      in platformio.ini, mirror the AMOLED-2.16 port's touch.cpp.
//   2. A minimal vendored reader — preferred when the only available
//      library is GPL-licensed (see boards/waveshare_amoled_18/touch.cpp).
//
// Whichever you pick, touch_hal_read() must complete in well under 5 ms
// (a single I2C burst is fine) so it doesn't drop frames.

static bool     touch_pressed = false;
static uint16_t touch_x = 0;
static uint16_t touch_y = 0;

static void touch_read_into_shared_state(void) {
    // Registeradresse setzen. Mit STOP statt Repeated-Start, weil das
    // auf diesem Board mit Arduino-ESP32 stabiler läuft.
    Wire.beginTransmission(TP_ADDR);
    Wire.write(0x02);

    if (Wire.endTransmission(true) != 0) {
        touch_pressed = false;
        return;
    }

    delayMicroseconds(100);

    if (Wire.requestFrom(TP_ADDR, (uint8_t)5, true) != 5) {
        touch_pressed = false;
        return;
    }

    const uint8_t fingers = Wire.read() & 0x0F;
    const uint8_t x_high = Wire.read();
    const uint8_t x_low  = Wire.read();
    const uint8_t y_high = Wire.read();
    const uint8_t y_low  = Wire.read();

    if (fingers == 0 || fingers > 5) {
        touch_pressed = false;
        return;
    }

    touch_x = ((uint16_t)(x_high & 0x0F) << 8) | x_low;
    touch_y = ((uint16_t)(y_high & 0x0F) << 8) | y_low;

    if (touch_x >= LCD_WIDTH) {
        touch_x = LCD_WIDTH - 1;
    }

    if (touch_y >= LCD_HEIGHT) {
        touch_y = LCD_HEIGHT - 1;
    }

    touch_pressed = true;
}

void touch_hal_init(void) {
    // FT3168 power-mode register 0xA5 = 0x00: active scanning.
    Wire.beginTransmission(TP_ADDR);
    Wire.write(0xA5);
    Wire.write(0x00);

    if (Wire.endTransmission() == 0) {
        Serial.println("FT3168 touch initialized");
    } else {
        Serial.println("FT3168 touch initialization failed");
    }
}

void touch_hal_read(uint16_t* x, uint16_t* y, bool* pressed) {
    touch_read_into_shared_state();

    *x = touch_x;
    *y = touch_y;
    *pressed = touch_pressed;
}
