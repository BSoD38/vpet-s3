#include "app.hpp"
#include "engine/gfx.hpp"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Care actions persist immediately (Pet::markSaved), so this only bounds how much
// passive decay/age is lost on a power cut. Kept long to spare NVS flash wear
// (a per-field dirty flag wouldn't help: decay changes state every tick).
static const float AUTOSAVE_SECS  = 120.0f;
static const float STATLOG_SECS   = 5.0f;
static const float TRANS_DUR      = 0.26f;  // menu slide duration (seconds)
static const float TRANS_IRIS_DUR = 0.55f;  // iris close+open duration (seconds)

// "back" easing curves (overshoot / anticipation) for the menu slides.
static float ease_out_back(float t) {           // overshoots past the target, settles
    const float c1 = 1.70158f, c3 = 2.70158f;
    float x = t - 1.0f;
    return 1.0f + c3 * x * x * x + c1 * x * x;
}
static float ease_in_back(float t) {            // winds back a touch, then accelerates away
    const float c1 = 1.70158f, c3 = 2.70158f;
    return c3 * t * t * t - c1 * t * t;
}

void App::setScene(SceneId id, Slide slide)
{
    // Freeze the current (outgoing) scene before swapping, then animate the new
    // one sliding in over the next TRANS_DUR seconds.
    if (slide != Slide::None) {
        gfx_snapshot();
        transitioning_ = true;
        transT_ = 0.0f;
        slide_ = slide;
    }
    curScene_ = id;
    switch (id) {
        case SceneId::Home:     scenes.set(&home);     break;
        case SceneId::Menu:     scenes.set(&menu);     break;
        case SceneId::Run:      scenes.set(&run);      break;
        case SceneId::Battle:       scenes.set(&battle);       break;
        case SceneId::BattleSelect: scenes.set(&battleSelect); break;
        case SceneId::Settings: scenes.set(&settings); break;
        case SceneId::TimeSet:  scenes.set(&timeset);  break;
        case SceneId::Stats:    scenes.set(&stats);    break;
        case SceneId::Rename:   scenes.set(&rename);   break;
    }
}

void App::init()
{
    gfx_init();                       // create the back-buffer (after Display_Init in main)
    home.bind(*this); menu.bind(*this); run.bind(*this); battle.bind(*this); battleSelect.bind(*this);
    settings.bind(*this); timeset.bind(*this);
    stats.bind(*this); rename.bind(*this);

    creatures.loadAll();              // load the creature roster before the pet resolves its id
    pet.boot();                       // load save + offline aging, or hatch a new egg
    debugOverlay = save.loadU8("dbg", 0) != 0;
    setScene(SceneId::Home);

    power_mark_display_ready();        // panel is up: sleep helpers may now touch it
    power_.begin(esp_timer_get_time());
}

void App::runLoop()
{
    Input in{};
    int64_t last = esp_timer_get_time();
    float saveAcc = 0, logAcc = 0;

    while (true) {
        int64_t now = esp_timer_get_time();
        float dt = (now - last) / 1e6f;
        if (dt > 0.1f) dt = 0.1f;     // clamp big gaps
        last = now;

        input.poll(in);

        // --- device power management (light/deep sleep, PWR button) ---
        // Runs before scene input so a wake-tap isn't also handled as a game tap.
        bool touchAct  = in.pressed || in.down;
        bool sceneIdle = (curScene_ == SceneId::Home) && !transitioning_;
        switch (power_.update(touchAct, sceneIdle, now)) {
            case PowerAction::EnterLight: power_enter_light(); break;
            case PowerAction::ExitLight:  power_exit_light();  break;
            case PowerAction::EnterDeep:  pet.markSaved(); power_enter_deep_sleep(); break;   // no return
            case PowerAction::PowerOff:   pet.markSaved(); power_off();               break;  // no return
            case PowerAction::None:       break;
        }
        if (power_.mode() == PowerMode::Light) {
            // Screen off: keep advancing the sim + timekeeping, but skip input/render.
            // A touch or PWR press is picked up next iteration and wakes us.
            pet.tick(dt * (float)pet.state().gameSpeed);
            saveAcc += dt;
            if (saveAcc >= AUTOSAVE_SECS) { saveAcc = 0; pet.markSaved(); }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (!transitioning_) scenes.input(in);         // ignore taps mid-slide

        pet.tick(dt * (float)pet.state().gameSpeed);   // speed multiplier applied here
        scenes.update(dt);
        scenes.render();                               // renders the (new) scene into fb

        if (transitioning_) {
            float dur = (slide_ == Slide::Iris) ? TRANS_IRIS_DUR : TRANS_DUR;
            transT_ += dt / dur;
            float t = transT_;
            bool done = (t >= 1.0f);
            if (done) t = 1.0f;

            if (slide_ == Slide::Iris) {
                const int Rmax = 205;   // > centre-to-corner (~200): fully covers the panel
                if (t < 0.5f) gfx_present_iris(true,  (int)(Rmax * (1.0f - t / 0.5f)));  // close old
                else          gfx_present_iris(false, (int)(Rmax * ((t - 0.5f) / 0.5f))); // open new
            } else if (slide_ == Slide::Forward) {     // new covers old, slides in from right (overshoot)
                float e = ease_out_back(t);
                gfx_present_cover((int)(GAME_W * (1.0f - e)), col::panel);   // panel = menu/settings bg
            } else {                                   // Back: old slides off right, revealing new
                float e = ease_in_back(t);
                gfx_present_reveal((int)(GAME_W * e), col::panel);   // panel = outgoing menu bg
            }
            if (done) transitioning_ = false;
        } else {
            gfx_present();
        }

        saveAcc += dt;
        if (saveAcc >= AUTOSAVE_SECS) { saveAcc = 0; pet.markSaved(); }

        logAcc += dt;
        if (logAcc >= STATLOG_SECS) {
            logAcc = 0;
            const PetState& p = pet.state();
            ESP_LOGI("GAME", "%s age=%.0f hun=%.0f hap=%.0f hp=%.0f poop=%u sick=%u",
                     pet.stageName(), p.ageSecs, p.hunger, p.happiness, p.health, p.poop, p.sick);
        }

        vTaskDelay(1);
    }
}
