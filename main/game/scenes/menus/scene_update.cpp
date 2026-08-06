#include "scene_update.hpp"
#include "core/app.hpp"
#include "engine/gfx.hpp"
#include "ui/widgets.hpp"
#include "esp_system.h"   // esp_restart

static const Rect kInstall{ 30, 214, GAME_W - 60, 52 };

// Painted directly from inside the (blocking) fw_apply loop -- the normal render pass
// doesn't run again until the install finishes, so this owns the whole frame.
// `stage` is "firmware" or a data partition label (creatures, gamedata).
static void draw_progress(const char* stage, int pct, void*)
{
    fb.fillScreen(col::panel);
    gfx_text(16, 16, 2, col::accent, "System Update");
    gfx_text(16, 108, 1, col::dim, "Installing");
    gfx_text(16, 122, 2, col::white, "%s  %d%%", stage, pct);
    Rect bar{ 20, 156, GAME_W - 40, 26 };
    bar.fill(rgb565(50, 54, 74), 8);
    if (pct > 0) fb.fillRoundRect(bar.x, bar.y, bar.w * pct / 100, bar.h, 8, col::good);
    bar.outline(col::white, 8);
    gfx_text(16, 200, 1, col::dim, "Keep the device powered.");
    gfx_present();
}

void SceneUpdate::onEnter()
{
    installing_ = false;
    done_       = false;
    rebootT_    = 0.0f;
    error_      = nullptr;
    probe_      = fw_probe(info_);
}

void SceneUpdate::update(float dt)
{
    if (done_) {
        // Let the success frame actually show before the panel goes dark.
        rebootT_ += dt;
        if (rebootT_ >= 1.5f) { app().pet.markSaved(); esp_restart(); }
        return;
    }
    if (!installing_) return;
    installing_ = false;

    app().pet.markSaved();          // sim state safe before we block the loop for ~30s
    draw_progress("firmware", 0, nullptr);
    error_ = fw_apply(&draw_progress, nullptr);
    if (!error_) { done_ = true; rebootT_ = 0.0f; }
}

// Shared header block for the Ok / UpToDate pages: versions, build stamp, and the
// per-piece status list from the manifest.
static void render_card_info(const FwInfo& info)
{
    gfx_text(16,  64, 1, col::dim,  "Installed");
    gfx_text(16,  78, 2, col::white, "v%s", info.curVersion);
    gfx_text(16, 108, 1, col::dim,  "On SD card");
    gfx_text(16, 122, 2, col::good, "v%s", info.version);
    gfx_text(16, 146, 1, col::dim,  "built %s", info.built);

    if (!info.hasManifest) {
        gfx_text(16, 162, 1, col::dim, "size  %u KB (no manifest: app only)", (unsigned)(info.size / 1024));
        if (info.sameVersion)
            gfx_text(16, 182, 1, col::accent, "This version is already installed.");
        return;
    }

    // One status line per piece; only what differs gets written.
    int y = 162;
    gfx_text(16, y, 1, info.appPending ? col::accent : col::dim, "firmware:  %s",
             info.appPending ? "will update" : "up to date");
    y += 13;
    for (int i = 0; i < info.dataCount; i++) {
        gfx_text(16, y, 1, info.data[i].pending ? col::accent : col::dim, "%s:  %s",
                 info.data[i].name, info.data[i].pending ? "will update" : "up to date");
        y += 13;
    }
}

void SceneUpdate::render()
{
    fb.fillScreen(col::panel);
    gfx_text(16, 16, 2, col::accent, "System Update");

    if (done_) {
        gfx_text(16, 120, 2, col::good, "Update installed!");
        gfx_text(16, 152, 1, col::white, "Restarting...");
        return;                     // no Back button: the reboot is already committed
    }

    draw_back();

    if (error_) {
        gfx_text(16, 72, 2, col::warn, "Install failed");
        gfx_text_wrap(16, 104, GAME_W - 32, 1, col::white, error_);
        gfx_text_wrap(16, 140, GAME_W - 32, 1, col::dim,
                      "The running firmware is untouched. "
                      "Fix the files on the card and try again. If a data write was "
                      "interrupted, it is finished automatically at the next boot "
                      "with the card in.");
        return;
    }

    switch (probe_) {
    case FwProbe::NoCard:
        gfx_text_wrap(16, 72, GAME_W - 32, 1, col::white,
                      "No SD card.\n\n"
                      "The card is detected at power-on: insert one with the update "
                      "files at its root, then restart the device.");
        break;
    case FwProbe::NoFile:
        gfx_text_wrap(16, 72, GAME_W - 32, 1, col::white,
                      "No update files on the card.\n\n"
                      "On the PC, package the build with:\n"
                      "python tools/make_update.py E:\\\n\n"
                      "then put the card back and re-enter this screen.");
        break;
    case FwProbe::BadImage:
        gfx_text_wrap(16, 72, GAME_W - 32, 1, col::warn,
                      "update.bin is not a valid firmware image for this device.");
        break;
    case FwProbe::WrongProject:
        gfx_text_wrap(16, 72, GAME_W - 32, 1, col::warn,
                      "The update on this card belongs to a different project.");
        break;
    case FwProbe::TooBig:
        gfx_text_wrap(16, 72, GAME_W - 32, 1, col::warn,
                      "update.bin is larger than the app slot.");
        break;
    case FwProbe::NoSlot:
        gfx_text_wrap(16, 72, GAME_W - 32, 1, col::warn,
                      "No inactive app slot found (partition table problem).");
        break;
    case FwProbe::UpToDate:
        render_card_info(info_);
        gfx_text(16, 224, 2, col::good, "Up to date");
        gfx_text_wrap(16, 252, GAME_W - 32, 1, col::dim,
                      "Everything on the card is already installed.");
        break;
    case FwProbe::Ok:
        render_card_info(info_);
        kInstall.button("INSTALL", col::good, col::black);
        gfx_text_wrap(16, 276, GAME_W - 32, 1, col::dim,
                      "Firmware goes to the spare slot; data packs are verified after writing.");
        break;
    }
}

void SceneUpdate::onInput(const Input& in)
{
    if (installing_ || done_) return;   // committed: ignore taps
    if (!in.pressed) return;

    if (kBack.contains(in)) { app().setScene(SceneId::Settings, Slide::Back); return; }
    if (probe_ == FwProbe::Ok && kInstall.contains(in)) installing_ = true;
}
