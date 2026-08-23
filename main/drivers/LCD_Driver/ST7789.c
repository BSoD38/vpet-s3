#include "ST7789.h"
#include <stdbool.h>

static const char *TAG_LCD = "ST7789";

esp_lcd_touch_handle_t tp = NULL;

// Which controller answered. The 2.8 ships with either, and it is the one reliable way to
// tell the two board revisions apart from software (About screen / bug reports).
const char *Touch_Model = "none";

// LovyanGFX (game/display.cpp) owns the SPI bus and ST7789 panel now.
// This module only owns the backlight (LEDC) and touch controller init.
void LCD_Init(void)
{
    Backlight_Init();

    esp_err_t ret = TOUCH_Init(&tp);
    if (ret == ESP_OK) {
        Touch_Model = "CST328";
    } else {
        ESP_LOGW("Touch", "CST328 init failed, try CST3530");
        ret = TOUCH2_Init(&tp);
        if (ret == ESP_OK) {
            Touch_Model = "CST3530";
        } else {
            ESP_LOGE("Touch", "CST3530 failed");
        }
    }
}

//////////////////////////////////////////////////////////////////////////////
// Backlight program

uint8_t LCD_Backlight = 70;
static ledc_channel_config_t ledc_channel;
static bool    s_bk_inited    = false;   // LEDC ready? (Set_Backlight is a no-op before this)
static bool    s_bk_suspended = false;   // device sleep forces the panel dark
static uint8_t s_bk_last      = 70;      // last level requested via Set_Backlight (restored on resume)

// Drive the LEDC duty for a 0..100 level (assumes LEDC is initialised).
static void bk_apply(uint8_t Light)
{
    if(Light > Backlight_MAX) Light = Backlight_MAX;
    uint16_t Duty = LEDC_MAX_Duty-(81*(Backlight_MAX-Light));
    if(Light == 0)
        Duty = 0;
    ledc_set_duty(ledc_channel.speed_mode, ledc_channel.channel, Duty);
    ledc_update_duty(ledc_channel.speed_mode, ledc_channel.channel);
}

void Backlight_Init(void)
{
    ESP_LOGI(TAG_LCD, "Init LCD backlight (LEDC)");
    gpio_config_t bk_gpio_config = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << EXAMPLE_PIN_NUM_BK_LIGHT
    };
    ESP_ERROR_CHECK(gpio_config(&bk_gpio_config));

    ledc_timer_config_t ledc_timer = {
        .duty_resolution = LEDC_TIMER_13_BIT,
        .freq_hz = 5000,
        .speed_mode = LEDC_LS_MODE,
        .timer_num = LEDC_HS_TIMER,
        .clk_cfg = LEDC_AUTO_CLK
    };
    ledc_timer_config(&ledc_timer);

    ledc_channel.channel    = LEDC_HS_CH0_CHANNEL;
    ledc_channel.duty       = 0;
    ledc_channel.gpio_num   = EXAMPLE_PIN_NUM_BK_LIGHT;
    ledc_channel.speed_mode = LEDC_LS_MODE;
    ledc_channel.timer_sel  = LEDC_HS_TIMER;
    ledc_channel_config(&ledc_channel);
    ledc_fade_func_install(0);

    s_bk_inited = true;
    Set_Backlight(LCD_Backlight);      //0~100
}
// Set the requested brightness. Before Backlight_Init this is a harmless no-op, so the
// headless sim (offline/deep-sleep catch-up) can call it via Pet::tick without a live LEDC.
void Set_Backlight(uint8_t Light)
{
    if(Light > Backlight_MAX) Light = Backlight_MAX;
    s_bk_last = Light;
    if(!s_bk_inited) return;
    bk_apply(s_bk_suspended ? 0 : Light);
}
// Device sleep forces the panel dark without disturbing the pet's scheduled brightness:
// Set_Backlight keeps recording the intended level while suspended; resume restores it.
void Backlight_Suspend(uint8_t on)
{
    s_bk_suspended = on ? true : false;
    if(!s_bk_inited) return;
    bk_apply(s_bk_suspended ? 0 : s_bk_last);
}
// end Backlight program
