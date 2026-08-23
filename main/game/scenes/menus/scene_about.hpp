#pragma once
#include "core/scene.hpp"
#include "ui/widgets.hpp"   // ListView

// About / system info: what firmware is running, what hardware it is running on, and the
// live power, clock, memory and mod-pack state. Reached from Settings -> SYSTEM.
//
// This is the screen to read out when something is wrong, so everything on it is measured
// rather than assumed (the touch controller, the flash and PSRAM sizes and the RTC's backup
// status are all read from the device) -- and it is the one place the battery voltage behind
// the HUD's gauge is visible.
class SceneAbout : public Scene {
public:
    void onEnter() override;
    void update(float dt) override;
    void render() override;
    void onInput(const Input& in) override;

private:
    enum Kind : uint8_t { Header, Field, Note };
    struct Row {
        Kind kind;
        char label[14];
        char value[40];
    };
    static const int MAX_ROWS = 52;

    Row      rows_[MAX_ROWS];
    int      n_ = 0;
    ListView list_;
    float    refresh_ = 0.0f;   // seconds until the live values are rebuilt
    int      convFiles_ = 0;    // loose-SD conversation files, counted on entry

    void add(Kind k, const char* label, const char* fmt, ...);
    void rebuild();             // re-read everything into rows_
    // Conversation mods are counted by walking the card, which is far too slow for the
    // refresh timer -- so it happens once, on entry, and the result is held here.
    int  countLooseConversations();
};
