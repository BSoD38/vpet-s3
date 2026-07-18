#pragma once
// Bridge to the existing C driver modules for use from the C++ game code.
extern "C" {
#include "ST7789.h"       // pins, EXAMPLE_LCD_*, Set_Backlight, LCD_Backlight, tp
#include "PCF85063.h"     // datetime_t, datetime (RTC)
#include "BAT_Driver.h"   // BAT_analogVolts
#include "QMI8658.h"      // Accel
#include "esp_lcd_touch.h"
}
