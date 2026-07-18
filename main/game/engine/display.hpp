#pragma once
#include <LovyanGFX.hpp>

// LovyanGFX device for the Waveshare ESP32-S3-Touch-LCD-2.8:
// ST7789 240x320 on SPI3. Owns the panel + SPI bus (replaces the old esp_lcd setup).
class LGFX : public lgfx::LGFX_Device {
    lgfx::Panel_ST7789 _panel;
    lgfx::Bus_SPI      _bus;
public:
    LGFX();
};

extern LGFX display;

// Initialize the panel (SPI + ST7789), clear to black. Call before turning on backlight.
void Display_Init();
