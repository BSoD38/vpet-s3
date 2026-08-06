#include "app.hpp"
#include "sim/gamedata.hpp"      // gamedata_json_use_psram (process-wide cJSON policy)
#include "engine/gfx.hpp"
#include "engine/fw_update.hpp"  // fw_confirm_running_image (post-update rollback cancel)
#include "engine/pakfs.hpp"      // base.pak + SD mod packs, mounted before the registries scan
#include "engine/sdwatch.hpp"    // mid-session SD card yank/insert detection
#include "ui/widgets.hpp"        // Rect (card-change halt screen button)
#include "esp_system.h"          // esp_restart (card-change halt screen)
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"

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

// Frame painter for fw_data_recovery: a rare, blocking boot path (only after a power cut
// mid data-partition write), so it owns the screen while it runs.
static void draw_boot_repair(const char* stage, int pct, void*)
{
    fb.fillScreen(col::panel);
    gfx_text(16, 120, 2, col::accent, "Repairing data");
    gfx_text(16, 152, 2, col::white, "%s  %d%%", stage, pct);
    gfx_text_wrap(16, 196, GAME_W - 32, 1, col::dim,
                  "An update was interrupted; finishing it from the SD card.");
    gfx_present();
}

// The saved creature's species isn't in the data installed this boot -- the card it came from
// is out, or a mod pack was uninstalled. Booting straight on would rewrite the pet as an egg
// and persist that immediately (Pet::boot ends in markSaved), destroying a creature the player
// could still have back for the cost of putting the data in place. So stop here and ask.
//
// Runs inside App::init, BEFORE Pet::boot(), so the simulation has not started and NOTHING has
// been written: restarting really does leave the pet exactly as it was. Returns only if the
// player accepts losing it, and the destructive choice is behind a confirm because a single
// mis-tap at boot must not be able to end a creature the player raised for weeks.
static void halt_for_missing_species(App& app, const char* id)
{
    ESP_LOGW("GAME", "saved species '%s' is not installed; asking before touching the save", id);

    const Rect kRestart{ 16, 196, GAME_W - 32, 50 };
    const Rect kFresh  { 16, 256, GAME_W - 32, 44 };
    const Rect kBack   { 16, 256, (GAME_W - 40) / 2, 44 };
    const Rect kWipe   { 24 + (GAME_W - 40) / 2, 256, (GAME_W - 40) / 2, 44 };

    char body[320];
    snprintf(body, sizeof body,
             "Your pet's species \"%s\" is missing from the data installed right now.\n\n"
             "It probably came from the SD card, or from a mod pack that has been removed. "
             "Nothing has been changed yet -- put that data back and restart, and your pet "
             "returns exactly as it was.", id);

    bool confirming = false;
    Input in{};
    while (true) {
        fb.fillScreen(col::panel);
        if (!confirming) {
            gfx_text(16, 28, 2, col::warn, "Creature missing");
            gfx_text_wrap(16, 60, GAME_W - 32, 1, col::white, body);
            kRestart.button("RESTART", col::accent, col::black);
            kFresh.button("START OVER", col::warn, col::black);
        } else {
            gfx_text(16, 28, 2, col::warn, "Give up on it?");
            gfx_text_wrap(16, 60, GAME_W - 32, 1, col::white,
                          "This deletes your saved pet for good and hatches a new egg. "
                          "Its age, bond, stats and journal go with it.\n\n"
                          "If there is any chance you can find that data again, restart "
                          "instead -- this cannot be undone.");
            kBack.button("BACK", col::accent, col::black);
            kWipe.button("DELETE", col::warn, col::black);
        }
        gfx_present();

        app.input.poll(in);
        if (in.pressed) {
            if (!confirming) {
                if (kRestart.contains(in)) esp_restart();
                if (kFresh.contains(in))   confirming = true;
            } else {
                if (kBack.contains(in)) confirming = false;
                if (kWipe.contains(in)) {
                    ESP_LOGW("GAME", "player gave up on missing species '%s'", id);
                    app.pet.forgetSave();   // next boot() hatches a fresh egg
                    return;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// The SD card was yanked or inserted mid-session. Everything SD-flavoured is boot-time
// state (the mount, creature/food/mod overlays, the update probe), so there is nothing
// sensible to resume: save, then hold a full-screen prompt until the player restarts --
// or do it for them after a countdown, so a device left unattended doesn't burn its
// battery showing a static message forever.
[[noreturn]] static void halt_for_card_change(App& app, bool inserted, bool screenOff)
{
    if (screenOff) power_exit_light();   // change arrived during screen-off light sleep
    app.pet.markSaved();
    ESP_LOGW("GAME", "SD card %s mid-session: halting for restart",
             inserted ? "inserted" : "removed");

    const Rect  kRestart{ 30, 210, GAME_W - 60, 52 };
    const float AUTO_S = 30.0f;
    const int64_t t0 = esp_timer_get_time();
    Input in{};
    while (true) {
        float left = AUTO_S - (float)(esp_timer_get_time() - t0) / 1e6f;
        if (left <= 0.0f) break;

        fb.fillScreen(col::panel);
        gfx_text(16, 56, 2, col::warn, inserted ? "SD card inserted" : "SD card removed");
        gfx_text_wrap(16, 96, GAME_W - 32, 1, col::white, inserted
            ? "The card is only picked up at power-on, so a restart is needed to "
              "use it.\n\nYour pet is saved."
            : "The game was using that card and can't continue safely without a "
              "restart. Put it back first if you want it available.\n\nYour pet is saved.");
        kRestart.button("RESTART NOW", col::accent, col::black);
        gfx_text(16, 280, 1, col::dim, "Restarting automatically in %ds", (int)left + 1);
        gfx_present();

        app.input.poll(in);
        if (in.pressed && kRestart.contains(in)) break;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    esp_restart();
}

// Everything a conversation gate is tested against. Built here so the conversation system
// stays decoupled from Pet. NOTE: gates compare IDS, never display names.
static ConvContext conv_ctx(const Pet& pet, const PersonalityTracker& drift)
{
    const PetState& p = pet.state();
    ConvContext c{};
    c.friendship = pet.friendship();
    c.wins       = pet.wins();
    c.stage      = p.stage;
    c.nature     = drift.natureId();
    c.trait      = drift.traitId();
    c.species    = pet.creature() ? pet.creature()->id : "";
    c.sick       = p.sick != 0;
    c.hungry     = care_tier(p.hunger) <= CARE_TIER_NEEDY;   // == the attention-badge threshold
    c.asleep     = p.lightsOff != 0;
    c.hour       = pet.simHour();
    c.mood       = pet.moodId();
    return c;
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
    switch (id) {
        case SceneId::Home:       scenes.set(&home);       break;
        case SceneId::Feed:       scenes.set(&feedScene);  break;
        case SceneId::Conversation: scenes.set(&conversationScene); break;
        case SceneId::Menu:       scenes.set(&menu);       break;
        case SceneId::Activities: scenes.set(&activities); break;
        case SceneId::Run:        scenes.set(&run);        break;
        case SceneId::MindMaze:   scenes.set(&mindmaze);   break;
        case SceneId::Smash:      scenes.set(&smash);      break;
        case SceneId::Bulwark:    scenes.set(&bulwark);    break;
        case SceneId::Stance:     scenes.set(&stance);     break;
        case SceneId::Battle:       scenes.set(&battle);       break;
        case SceneId::BattleSelect: scenes.set(&battleSelect); break;
        case SceneId::Settings: scenes.set(&settings); break;
        case SceneId::Update:   scenes.set(&updateScene); break;
        case SceneId::Cheats:   scenes.set(&cheats);   break;
        case SceneId::TimeSet:  scenes.set(&timeset);  break;
        case SceneId::Stats:    scenes.set(&stats);    break;
        case SceneId::Journal:  scenes.set(&journal);  break;
        case SceneId::Rename:   scenes.set(&rename);   break;
    }
}

void App::init()
{
    gfx_init();                       // create the back-buffer (after Display_Init in main)
    home.bind(*this); feedScene.bind(*this); conversationScene.bind(*this);
    menu.bind(*this); activities.bind(*this); run.bind(*this); mindmaze.bind(*this);
    smash.bind(*this); bulwark.bind(*this); stance.bind(*this);
    battle.bind(*this); battleSelect.bind(*this);
    settings.bind(*this); updateScene.bind(*this); cheats.bind(*this); timeset.bind(*this);
    stats.bind(*this); journal.bind(*this); rename.bind(*this);

    // Process-wide, and must precede every parse below: keeps JSON trees out of internal heap.
    gamedata_json_use_psram();

    // Finish any interrupted data-partition update BEFORE that data is mounted or parsed
    // (needs the cJSON hooks above for the manifest; usually a no-op).
    fw_data_recovery(&draw_boot_repair, nullptr);

    // Packs register /pakN roots that every registry below adds to its scan list, so they must
    // all exist before the first loadAll(). Boot-time-only, like the rest of the card state (a
    // mid-session card change halts for a restart anyway -- see halt_for_card_change).
    //
    // ORDER IS THE OVERRIDE RULE: /pak0 is weakest, so the base game's own pack must mount
    // FIRST or a player's mod could not replace anything in it. The base data lives in the
    // gamedata partition as a single base.pak (built by tools/make_paks.py), hence the mount
    // here rather than inside a registry -- pakfs has to see the filesystem before any
    // registry asks it for a root.
    gamedata_mount();                 // idempotent; registries call it again harmlessly
    pakfs_mount_all(GAMEDATA_ROOT);   // base.pak  -> /pak0
    pakfs_mount_all("/sdcard/mods");  // user mods -> /pak1..

    creatures.loadAll();              // load the creature roster before the pet resolves its id
    foods.loadAll();                  // food list (independent of the pet; needed by the Feed picker)
    personalities.loadAll();          // natures + traits, before any drift is evaluated
    drift.boot();
    conversations.init(save);         // mounts gamedata + loads facts/seen history
    pet.setDriftSink(&drift);         // BEFORE pet.boot(): offline catch-up ticks the drift too
    // ...and before it for a second reason: the registries above are what decide whether the
    // saved creature still exists. If it doesn't, ask rather than let boot() quietly rewrite
    // it to an egg and save that. Returns only if the player chooses to start over.
    char missingId[24];
    if (pet.savedSpeciesMissing(missingId, sizeof missingId))
        halt_for_missing_species(*this, missingId);
    pet.boot();                       // load save + offline aging, or hatch a new egg
    // A brand-new creature starts with a blank slate: its predecessor's conversation history
    // and journal don't belong to it. Player FACTS survive -- they're about the player.
    if (pet.startedFresh()) conversations.clearSeen();
    debugOverlay = save.loadU8("dbg", 0) != 0;
    setScene(SceneId::Home);

    power_mark_display_ready();        // panel is up: sleep helpers may now touch it
    power_.begin(esp_timer_get_time());

    // Init finished = this firmware is demonstrably alive. On the first boot after an
    // SD/OTA update this cancels the bootloader's pending rollback; a crash anywhere
    // above would instead have rebooted into the PREVIOUS firmware. No-op otherwise.
    fw_confirm_running_image();
}

void App::runLoop()
{
    Input in{};
    int64_t last = esp_timer_get_time();
    float saveAcc = 0, logAcc = 0, fps = 0.0f;

    while (true) {
        int64_t now = esp_timer_get_time();
        float dt = (now - last) / 1e6f;
        if (dt > 0.1f) dt = 0.1f;     // clamp big gaps
        last = now;

        input.poll(in);

        // Mid-session SD presence change: halt on a restart prompt. Checked BEFORE the
        // light-sleep branch below, so a yank during screen-off wakes into the prompt
        // instead of silently breaking the mounted card.
        if (sdwatch_change() != SdChange::None)
            halt_for_card_change(*this, sdwatch_change() == SdChange::Inserted,
                                 power_.mode() == PowerMode::Light);   // never returns

        // --- device power management (light/deep sleep, PWR button) ---
        // Runs before scene input so a wake-tap isn't also handled as a game tap.
        // Scenes opt out of sleeping (minigame/battle) via Scene::allowsSleep().
        bool   touchAct     = in.pressed || in.down;
        Scene* cs           = scenes.current();
        bool   sleepAllowed = (!cs || cs->allowsSleep()) && !transitioning_;
        // Per-scene care attenuation (0..1), orthogonal to gameSpeed: in-play scenes
        // (minigame/battle) slow or freeze the pet sim so a session doesn't neglect it.
        float  careMult     = cs ? cs->careSpeed() : 1.0f;
        switch (power_.update(touchAct, sleepAllowed, now)) {
            case PowerAction::EnterLight: power_enter_light(); break;
            case PowerAction::ExitLight:  power_exit_light();  break;
            case PowerAction::EnterDeep:  pet.markSaved(); power_enter_deep_sleep(); break;   // no return
            case PowerAction::PowerOff:   pet.markSaved(); power_off();               break;  // no return
            case PowerAction::None:       break;
        }
        if (power_.mode() == PowerMode::Light) {
            // Screen off: keep advancing the sim + timekeeping, but skip input/render.
            // A touch or PWR press is picked up next iteration and wakes us.
            pet.tick(dt * (float)pet.state().gameSpeed * careMult);
            saveAcc += dt;
            if (saveAcc >= AUTOSAVE_SECS) { saveAcc = 0; pet.markSaved(); }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (!transitioning_) scenes.input(in);         // ignore taps mid-slide

        pet.tick(dt * (float)pet.state().gameSpeed * careMult);   // gameSpeed × per-scene care factor
        // Time-sliced conversation selection: one file per frame, off the render path, so a
        // large modded library never costs a frame. Real time (not gameSpeed) on purpose.
        // MUST stay below the light-sleep `continue` above: with the screen off there is nobody
        // to show a bubble to, and scanning would just burn battery. Scanning is additionally
        // paused (allowScan=false) in the scenes that forbid sleeping -- minigames and battle
        // are timing-critical, and one scan file costs ~7-12 ms of FAT+parse in a frame.
        conversations.update(dt, conv_ctx(pet, drift), sleepAllowed);
        scenes.update(dt);
        scenes.render();                               // renders the (new) scene into fb

        if (debugOverlay && dt > 0.0f) {               // on-screen FPS (Settings > System > Debug info)
            fps += (1.0f / dt - fps) * 0.08f;          // smoothed
            gfx_text(2, 2, 1, col::warn, "%.0f fps", fps);
        }

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
            ESP_LOGI("MEM", "free internal %u KB, psram %u KB",
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
        }

        vTaskDelay(1);
    }
}
