#pragma once
#include "core/scene.hpp"

// Settings: four tabbed pages. GAME = care freeze. SOUND = master/music/effects volume and
// mute. SCREEN = how long the screen stays on and how bright it is. SYSTEM = debug overlay
// toggle, set time/date, cheats screen, update, SD card, about, factory reset. Reached from
// Menu -> Settings. (Game speed moved to the Cheats screen: it is a testing aid, not a
// player option.)
class SceneSettings : public Scene {
    int page_ = 0;   // 0 = Game, 1 = Sound, 2 = Screen, 3 = System
    // The SD Card sub-page (status + the only format in the firmware). Shares holdT_ with
    // the factory-reset page below: both commit on a hold, and only one can be open at a
    // time -- each is entered from a SYSTEM row and clears holdT_ entering and leaving.
    bool    sdPage_ = false;
    // Factory-reset confirmation: a dedicated sub-page with a HOLD-to-erase button, so a
    // stray double-tap can never wipe the save. holdT_ accumulates while the finger stays
    // on the button and resets the moment it leaves.
    bool    confirmReset_ = false;
    float   holdT_ = 0.0f;
    bool    down_ = false;         // live touch state (mirrored from onInput for update())
    int16_t tx_ = 0, ty_ = 0;
    // Which volume slider the finger grabbed, or -1. Latched on press so a drag keeps
    // controlling the slider it started on even when the finger wanders off it vertically,
    // and so the new value is only written to NVS once, on release.
    int8_t  dragSlider_ = -1;
    // The brightness slider, latched the same way and for the same reasons. Kept separate
    // from dragSlider_ rather than folded in as a fourth index: it lives on another page and
    // reads/writes the panel, not the mixer.
    bool    dragBright_ = false;
public:
    void onEnter() override;
    void update(float dt) override;
    void render() override;
    void onInput(const Input& in) override;
};
