#include "ST7789.h"

static const char *TAG_LCD = "ST7789";

esp_lcd_touch_handle_t tp = NULL;

// LovyanGFX (game/display.cpp) owns the SPI bus and ST7789 panel now.
// This module only owns the backlight (LEDC) and touch controller init.
void LCD_Init(void)
{
    Backlight_Init();

    esp_err_t ret = TOUCH_Init(&tp);
    if (ret != ESP_OK) {
        ESP_LOGW("Touch", "CST328 init failed, try CST3530");
        ret = TOUCH2_Init(&tp);
        if (ret != ESP_OK) {
            ESP_LOGE("Touch", "CST3530 failed");
        }
    }
}

//////////////////////////////////////////////////////////////////////////////
// Backlight program

uint8_t LCD_Backlight = 70;
static ledc_channel_config_t ledc_channel;
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

    Set_Backlight(LCD_Backlight);      //0~100
}
void Set_Backlight(uint8_t Light)
{
    if(Light > Backlight_MAX) Light = Backlight_MAX;
    uint16_t Duty = LEDC_MAX_Duty-(81*(Backlight_MAX-Light));
    if(Light == 0)
        Duty = 0;
    ledc_set_duty(ledc_channel.speed_mode, ledc_channel.channel, Duty);
    ledc_update_duty(ledc_channel.speed_mode, ledc_channel.channel);
}
// end Backlight program
