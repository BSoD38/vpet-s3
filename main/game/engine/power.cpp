#include "power.hpp"
#include "sim/pet.hpp"
#include "sim/save.hpp"
#include "sim/creatures.hpp"
#include "sim/personality.hpp"    // drift must ride along on headless catch-up
#include "engine/display.hpp"     // display.sleep() / display.wakeup()

#include "esp_log.h"
#include "esp_sleep.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"        // rtc_gpio_* (keep the PWR key pulled up through deep sleep)
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern "C" {
#include "PWR_Key.h"              // PWR_Key_Down, Shutdown, PWR_*_PIN
#include "ST7789.h"               // Set_Backlight, Backlight_Suspend
}

static const char* TAG = "PWR";

// ---- button gesture timing (classified on release) ----
static const int64_t HOLD_DEEP_US = 800LL  * 1000;   // >= this hold -> deep sleep
static const int64_t HOLD_OFF_US  = 3000LL * 1000;   // >= this hold -> power off
static const int64_t AUTO_LIGHT_US = 180LL  * 1000000;   // idle this long on Home -> light sleep
static const int64_t AUTO_DEEP_US  = 900LL  * 1000000;   // total idle -> escalate light -> deep sleep

// ---- deep-sleep wake triggers (with hysteresis so a stat that sits low doesn't
//      re-wake the screen every poll: alert once on the way down, re-arm above CLR) ----
static const float HUNGER_WAKE = 15.0f, HUNGER_CLR = 25.0f;
static const float HAPPY_WAKE  = 20.0f, HAPPY_CLR  = 30.0f;
static const float HP_WAKE     = 20.0f, HP_CLR     = 30.0f;
static const uint8_t A_HUN = 0x1, A_HAP = 0x2, A_HP = 0x4;   // persisted alert bits ("slpAlert")

static bool s_display_ready = false;   // is the LovyanGFX panel initialised?

void power_mark_display_ready() { s_display_ready = true; }

// ---------------------------------------------------------------------------
// Boot-path helpers
// ---------------------------------------------------------------------------

void power_early_init()
{
    // A deep-sleep wake resumes with pads still frozen by the hold latch. Release the
    // holds so we can drive the pins again, then keep the board powered.
    gpio_deep_sleep_hold_dis();
    gpio_hold_dis((gpio_num_t)PWR_Control_PIN);

    gpio_reset_pin((gpio_num_t)PWR_Control_PIN);
    gpio_set_direction((gpio_num_t)PWR_Control_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)PWR_Control_PIN, 1);            // latch on

    gpio_reset_pin((gpio_num_t)PWR_KEY_Input_PIN);
    gpio_set_direction((gpio_num_t)PWR_KEY_Input_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode((gpio_num_t)PWR_KEY_Input_PIN, GPIO_PULLUP_ONLY);
}

bool power_woke_from_timer()
{
    return esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER;
}

void power_service_timer_wake(SaveStore& save)
{
    // Stage before catch-up, so we can spot an evolution that happened during sleep.
    PetState pre{};
    uint8_t oldStage = save.load(pre) ? pre.stage : 0;

    // Advance the sim over the elapsed real time using the SAME path as a normal boot.
    // Heap-allocated: the registries are ~15 KB and app_main's stack is small.
    CreatureRegistry* reg = new CreatureRegistry();
    reg->loadAll();
    // The personality tracker MUST ride along, exactly as App::init wires it before
    // pet.boot(): this headless path replays the elapsed window and persists lastUpdate,
    // so any drift it skips is gone forever. Since an idle device lives almost entirely
    // in these 15-min wake slices, omitting the sink made days of neglect produce zero
    // idle drift -- the withdrawn traits were unreachable in the one scenario that earns
    // them. (idleSecs_ is persisted across the slices, so periods complete correctly.)
    PersonalityRegistry* preg = new PersonalityRegistry();
    preg->loadAll();
    PersonalityTracker* drift = new PersonalityTracker(save, *preg);
    drift->boot();
    Pet* pet = new Pet(save, *reg);
    pet->setDriftSink(drift);
    pet->boot();                    // seeds RTC, loads save, replays tick() + persists (markSaved)
    const PetState& p = pet->state();

    // Edge-detected threshold alerts. The mask lives OUTSIDE the versioned pet blob
    // (its own NVS key) so adding this never invalidates an existing save.
    uint8_t mask = save.loadU8("slpAlert", 0);
    uint8_t fired = 0;
    auto edge = [&](bool below, bool above, uint8_t bit) {
        if (below && !(mask & bit)) { fired |= bit; mask |= bit; }
        else if (above)             { mask &= (uint8_t)~bit; }
    };
    edge(p.hunger    < HUNGER_WAKE, p.hunger    > HUNGER_CLR, A_HUN);
    edge(p.happiness < HAPPY_WAKE,  p.happiness > HAPPY_CLR,  A_HAP);
    edge(p.health    < HP_WAKE,     p.health    > HP_CLR,     A_HP);
    save.storeU8("slpAlert", mask);

    bool evolved = (p.stage > oldStage);
    bool wake    = evolved || (fired != 0);

    ESP_LOGI(TAG, "timer wake: hun=%.0f hap=%.0f hp=%.0f stage=%u(<-%u) fired=0x%x -> %s",
             p.hunger, p.happiness, p.health, p.stage, oldStage,
             fired, wake ? "WAKE" : "re-sleep");

    delete pet;
    delete drift;
    delete preg;
    delete reg;

    if (!wake)
        power_enter_deep_sleep(POWER_DEEP_POLL_S);   // no return
    // else: fall through — app_main proceeds to a full display wake.
}

// ---------------------------------------------------------------------------
// Sleep / backlight actions
// ---------------------------------------------------------------------------

void power_enter_light()
{
    Backlight_Suspend(1);
    if (s_display_ready) display.sleep();      // ST7789 SLPIN (panel off)
    ESP_LOGI(TAG, "light sleep (screen off, sim running)");
}

void power_exit_light()
{
    if (s_display_ready) display.wakeup();      // SLPOUT + DISPON
    Backlight_Suspend(0);                        // restore the pet's scheduled brightness
    ESP_LOGI(TAG, "wake from light sleep");
}

void power_enter_deep_sleep(uint32_t interval_s)
{
    ESP_LOGI(TAG, "deep sleep: %us timer + PWR-key wake", (unsigned)interval_s);

    Backlight_Suspend(1);
    if (s_display_ready) display.sleep();
    vTaskDelay(pdMS_TO_TICKS(20));

    // Hold the power latch high through deep sleep so the board stays powered.
    gpio_set_level((gpio_num_t)PWR_Control_PIN, 1);
    gpio_hold_en((gpio_num_t)PWR_Control_PIN);
    gpio_deep_sleep_hold_en();

    // Don't instantly re-wake on the very press that requested sleep.
    while (PWR_Key_Down()) vTaskDelay(pdMS_TO_TICKS(20));
    vTaskDelay(pdMS_TO_TICKS(60));

    esp_sleep_enable_timer_wakeup((uint64_t)interval_s * 1000000ULL);
    // ESP32-S3 has RTC IO, so a PWR-key (active-low) wake goes through EXT1. Keep the pad
    // pulled up in the RTC domain so it idles high and a press pulls it low to wake.
    rtc_gpio_pullup_en((gpio_num_t)PWR_KEY_Input_PIN);
    rtc_gpio_pulldown_dis((gpio_num_t)PWR_KEY_Input_PIN);
    esp_sleep_enable_ext1_wakeup(1ULL << PWR_KEY_Input_PIN, ESP_EXT1_WAKEUP_ANY_LOW);
    esp_deep_sleep_start();

    for (;;) { }   // esp_deep_sleep_start() does not return
}

void power_off()
{
    ESP_LOGI(TAG, "power off");
    Shutdown();                       // battery: releases the latch -> board powers off here
    power_enter_deep_sleep(POWER_DEEP_POLL_S);   // USB fallback: sleep instead of a black brick
}

// ---------------------------------------------------------------------------
// Active-loop power manager
// ---------------------------------------------------------------------------

void PowerManager::begin(int64_t nowUs)
{
    lastActUs_   = nowUs;
    btnPrev_     = PWR_Key_Down();    // if we woke by holding the key, it may still be down
    btnConsumed_ = btnPrev_;          // ignore that initial hold until it's released
    btnDownUs_   = nowUs;
}

PowerAction PowerManager::update(bool touchActivity, bool sleepAllowed, int64_t nowUs)
{
    const bool btn         = PWR_Key_Down();
    const bool pressEdge   = btn && !btnPrev_;
    const bool releaseEdge = !btn && btnPrev_;

    if (pressEdge) { btnDownUs_ = nowUs; btnConsumed_ = false; }
    if (touchActivity || btn) lastActUs_ = nowUs;   // any input defers auto-sleep

    PowerAction act = PowerAction::None;

    if (mode_ == PowerMode::Light) {
        // Wake on any touch or PWR press. (A scene can't switch during light sleep -- App
        // skips scene input/update while the screen is off -- so sleepAllowed can't flip
        // here.) The waking press is consumed so it can't also read as a hold gesture in Active.
        if (touchActivity || pressEdge) {
            if (pressEdge) btnConsumed_ = true;
            mode_ = PowerMode::Active;
            act = PowerAction::ExitLight;
        } else if (!btn && (nowUs - lastActUs_) >= AUTO_DEEP_US) {
            // Sustained inactivity: escalate to deep sleep (light sleep still runs the CPU,
            // so this is where the real battery savings kick in). App persists + sleeps.
            act = PowerAction::EnterDeep;
        }
    } else {   // Active
        if (releaseEdge && !btnConsumed_) {
            int64_t held = nowUs - btnDownUs_;
            if      (held >= HOLD_OFF_US) act = PowerAction::PowerOff;   // always available
            else if (sleepAllowed)                                      // sleep gestures gated
                act = (held >= HOLD_DEEP_US) ? PowerAction::EnterDeep : PowerAction::EnterLight;
        }
        if (releaseEdge) btnConsumed_ = false;

        // Auto light-sleep after sustained inactivity, where the scene permits it and
        // never mid-press.
        if (act == PowerAction::None && sleepAllowed && !btn &&
            (nowUs - lastActUs_) >= AUTO_LIGHT_US) {
            act = PowerAction::EnterLight;
        }

        if (act == PowerAction::EnterLight) mode_ = PowerMode::Light;
    }

    btnPrev_ = btn;
    return act;
}
