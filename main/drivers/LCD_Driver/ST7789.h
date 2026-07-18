#pragma once
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/ledc.h"

// LCD panel wiring. LovyanGFX (game/display.cpp) owns the SPI bus + ST7789 panel;
// these defines are shared with that config and with the touch drivers.
#define EXAMPLE_PIN_NUM_MISO           -1
#define EXAMPLE_PIN_NUM_MOSI           45
#define EXAMPLE_PIN_NUM_SCLK           40
#define EXAMPLE_PIN_NUM_LCD_CS         42
#define EXAMPLE_PIN_NUM_LCD_DC         41
#define EXAMPLE_PIN_NUM_LCD_RST        39
#define EXAMPLE_PIN_NUM_BK_LIGHT       5
// Panel pixel dimensions (portrait)
#define EXAMPLE_LCD_H_RES              240
#define EXAMPLE_LCD_V_RES              320

#define Offset_X 0
#define Offset_Y 0

// Backlight (LEDC PWM on EXAMPLE_PIN_NUM_BK_LIGHT)
#define LEDC_HS_TIMER          LEDC_TIMER_0
#define LEDC_LS_MODE           LEDC_LOW_SPEED_MODE
#define LEDC_HS_CH0_CHANNEL    LEDC_CHANNEL_0
#define LEDC_ResolutionRatio   LEDC_TIMER_13_BIT
#define LEDC_MAX_Duty          ((1 << LEDC_ResolutionRatio) - 1)
#define Backlight_MAX   100

// Touch drivers (pull in esp_lcd_touch_handle_t and TOUCH_Init / TOUCH2_Init).
#include "CST328.h"
#include "CST3530.h"

extern esp_lcd_touch_handle_t tp;
extern uint8_t LCD_Backlight;

void Backlight_Init(void);           // init LEDC backlight (called by LCD_Init)
void Set_Backlight(uint8_t Light);   // 0..100 brightness
void LCD_Init(void);                 // backlight + touch init (LovyanGFX owns the panel)
