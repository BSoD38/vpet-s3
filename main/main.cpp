#include "engine/display.hpp"   // C++ (LovyanGFX)
#include "core/game.hpp"
#include "sim/save.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Existing C drivers — give them C linkage when included from C++.
extern "C" {
#include "I2C_Driver.h"
#include "ST7789.h"
#include "PCF85063.h"
#include "QMI8658.h"
#include "SD_MMC.h"
#include "Wireless.h"
#include "BAT_Driver.h"
#include "PWR_Key.h"
#include "PCM5101.h"
}

static const char *TAG = "MAIN";

// Background sensor/power task (unchanged): keeps datetime / battery / power state fresh.
static void Driver_Loop(void *parameter)
{
    Wireless_Init();
    while (1) {
        QMI8658_Loop();
        PCF85063_Loop();
        BAT_Get_Volts();
        PWR_Loop();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void Driver_Init(void)
{
    PWR_Init();
    BAT_Init();
    I2C_Init();
    PCF85063_Init();
    QMI8658_Init();
    Flash_Searching();
    xTaskCreatePinnedToCore(Driver_Loop, "Other Driver task", 4096, NULL, 3, NULL, 0);
}

static void game_task(void *arg)
{
    game_run();   // never returns
}

extern "C" void app_main(void)
{
    save_init();      // init NVS before any task uses it (beats the Wi-Fi task's init)
    Driver_Init();
    SD_Init();
    Audio_Init();

    Display_Init();   // LovyanGFX: SPI + ST7789 panel, cleared to black
    LCD_Init();       // backlight on + touch (CST328/CST3530)

    ESP_LOGI(TAG, "virtual-pet: Milestone 1 (care loop)");

    // Game/render loop on core 1 (drivers + Wi-Fi live on core 0).
    xTaskCreatePinnedToCore(game_task, "game", 8192, NULL, 5, NULL, 1);
}
