#include "../../hal/power_hal.h"
#include "board.h"
#include <Arduino.h>

#define PWR_LONG_MS 1500

static bool last_state = false;
static bool long_fired = false;
static bool long_flag = false;
static bool release_flag = false;
static uint32_t pressed_since = 0;

void power_hal_init(void) {
    pinMode(BTN_BACK_GPIO, INPUT_PULLUP);
}

void power_hal_tick(void) {
    const bool pressed = digitalRead(BTN_BACK_GPIO) == LOW;
    const uint32_t now = millis();

    if (pressed && !last_state) {
        pressed_since = now;
        long_fired = false;
    }

    if (pressed && !long_fired &&
        now - pressed_since >= PWR_LONG_MS) {
        long_flag = true;
        long_fired = true;
    }

    if (!pressed && last_state) {
        release_flag = true;
    }

    last_state = pressed;
}

int power_hal_battery_pct(void) {
    return -1;
}

bool power_hal_is_charging(void) {
    return false;
}

bool power_hal_is_vbus_in(void) {
    return false;
}

bool power_hal_pwr_pressed(void) {
    return false;
}

bool power_hal_pwr_long_pressed(void) {
    if (long_flag) {
        long_flag = false;
        return true;
    }
    return false;
}

bool power_hal_pwr_released(void) {
    if (release_flag) {
        release_flag = false;
        return true;
    }
    return false;
}