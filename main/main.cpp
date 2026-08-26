#include "engine/display.hpp"   // C++ (LovyanGFX)
#include "engine/power.hpp"     // sleep/power boot-path helpers
#include "engine/sdwatch.hpp"   // mid-session SD card yank/insert watcher
#include "engine/battery.hpp"   // charge gauge + charger detection (fed by the driver task)
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
}

static const char *TAG = "MAIN";

// Background sensor task: keeps datetime / battery fresh. (Power/PWR-button semantics
// moved to the game loop via PowerManager, so this no longer polls the button.)
static void Driver_Loop(void *parameter)
{
    Wireless_Init();
    while (1) {
        QMI8658_Loop();
        PCF85063_Loop();
        BAT_Get_Volts();
        // The gauge lives on this task rather than in the game loop because plug-in
        // detection wants an even sample interval, which a frame-rate-driven caller
        // cannot promise (and a minigame frame drop must not read as a voltage step).
        battery_poll(0.1f);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// I2C + RTC are brought up separately (in app_main) because the deep-sleep timer-wake
// path needs the RTC before deciding whether to do a full boot at all.
static void Driver_Init(void)
{
    PWR_Init();
    BAT_Init();
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
    power_early_init();           // release deep-sleep pad holds + keep the board latched on
    save_init();                  // NVS (before any task uses it; beats the Wi-Fi task's init)

    // Minimal bring-up shared by both boot paths: I2C + the battery-backed RTC.
    I2C_Init();
    PCF85063_Init();

    // Periodic deep-sleep wake: advance care/timekeeping headless and test the wake
    // triggers. Returns only if something fired (otherwise it re-sleeps in here); a
    // PWR-key wake / cold boot skips this and goes straight to a full wake.
    if (power_woke_from_timer()) {
        SaveStore save;
        power_service_timer_wake(save);
        ESP_LOGI(TAG, "wake trigger fired during sleep -> full wake");
    }

    Driver_Init();                // PWR latch/key, battery, IMU, flash search, sensor task
    SD_Init();
    sdwatch_start();              // baseline = SD_Init's outcome; game loop halts on change
    // NOTE: audio is brought up in App::init(), not here -- the mixer is useless until the
    // mod packs are mounted and the sound bank has been scanned out of them.

    // Screen brightness + timeout, BEFORE the backlight comes up: Backlight_Init() lights
    // the panel at LCD_Backlight, so loading first is the difference between the panel
    // opening at the player's level and flashing the default before correcting itself.
    { SaveStore save; screen_settings_load(save); }

    Display_Init();   // LovyanGFX: SPI + ST7789 panel, cleared to black
    LCD_Init();       // backlight on + touch (CST328/CST3530)

    ESP_LOGI(TAG, "virtual-pet: care loop + sleep/power online");

    // Game/render loop on core 1 (drivers + Wi-Fi live on core 0).
    xTaskCreatePinnedToCore(game_task, "game", 8192, NULL, 5, NULL, 1);
}
