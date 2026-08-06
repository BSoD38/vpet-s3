#include "engine/sdwatch.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <atomic>

extern "C" {
#include "SD_MMC.h"
}

static const char* TAG = "SDWATCH";

static std::atomic<int> s_change{ (int)SdChange::None };

static void watch_task(void*)
{
    sdmmc_card_t* card = SD_GetCard();
    const bool mounted = (card != nullptr);

    if (!mounted) {
        // The insertion probe re-negotiates with an absent card every cycle; silence the
        // driver's per-attempt error spam (boot's own SD messages have already printed).
        esp_log_level_set("sdmmc_common", ESP_LOG_NONE);
        esp_log_level_set("sdmmc_sd",     ESP_LOG_NONE);
        esp_log_level_set("sdmmc_req",    ESP_LOG_NONE);
    }

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(mounted ? 1000 : 2500));
        if (mounted) {
            // SEND_STATUS costs microseconds and serializes with real IO on the driver's
            // internal lock, so it can't corrupt an in-flight read.
            if (sdmmc_get_status(card) == ESP_OK) continue;
            vTaskDelay(pdMS_TO_TICKS(120));               // ride out a transient glitch
            if (sdmmc_get_status(card) == ESP_OK) continue;
            ESP_LOGW(TAG, "mounted SD card no longer answers -> removed");
            s_change = (int)SdChange::Removed;
            break;
        } else if (SD_Probe_Insertion()) {
            ESP_LOGW(TAG, "SD card detected mid-session -> inserted");
            s_change = (int)SdChange::Inserted;
            break;
        }
    }
    vTaskDelete(nullptr);   // latched; the game is halting -- only a reboot resets this
}

void sdwatch_start()
{
    xTaskCreatePinnedToCore(watch_task, "sdwatch", 4096, nullptr, 2, nullptr, 0);
}

SdChange sdwatch_change()
{
    return (SdChange)s_change.load();
}
