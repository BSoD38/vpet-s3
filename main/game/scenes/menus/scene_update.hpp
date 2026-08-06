#pragma once
#include "core/scene.hpp"
#include "engine/fw_update.hpp"

// System Update: install a new firmware from the SD card (/sdcard/update.bin, packaged by
// tools/make_update.py). Probes the card on entry, shows installed vs on-card version, and
// streams the image into the inactive app slot with a progress bar; on success the device
// reboots into the new firmware. Reached from Settings -> SYSTEM.
class SceneUpdate : public Scene {
    FwProbe     probe_ = FwProbe::NoCard;
    FwInfo      info_{};
    bool        installing_ = false;    // latched by the INSTALL tap; runs on the next update()
    bool        done_       = false;    // installed OK, reboot countdown running
    float       rebootT_    = 0.0f;
    const char* error_      = nullptr;  // static string from fw_apply, or null
public:
    void onEnter() override;
    void update(float dt) override;
    void render() override;
    void onInput(const Input& in) override;
    // Flashing must never race the sleep machinery (screen-off mid-install would look like
    // a hang; deep sleep would abort the write).
    bool allowsSleep() const override { return false; }
};
