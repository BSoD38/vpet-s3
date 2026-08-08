#pragma once
#include "engine/input.hpp"
#include "engine/audio/sfx.hpp"   // the default music id lives with the other sound ids

class App;   // scenes reach the game context (pet, gfx, scene switching) through this

// Base class for game screens. Scenes draw into the shared back-buffer and reach
// the rest of the game through app().
class Scene {
protected:
    App* app_ = nullptr;
public:
    virtual ~Scene() {}
    void bind(App& a) { app_ = &a; }   // called once by App during setup
    App& app() { return *app_; }

    virtual void onEnter() {}
    virtual void onExit() {}
    virtual void update(float dt) { (void)dt; }
    virtual void render() {}
    virtual void onInput(const Input& in) { (void)in; }

    // May the device sleep (light/deep, auto or via the PWR button) while this scene is
    // active? Default yes; timed/active scenes (minigame, battle) override to false so a
    // nap or an idle timeout can't interrupt play. (Power-off is still allowed.)
    virtual bool allowsSleep() const { return true; }

    // How fast the pet CARE simulation advances while this scene is active, as a fraction
    // in [0,1]. This is ORTHOGONAL to the user's gameSpeed multiplier: the loop ticks the
    // sim by dt * gameSpeed * careSpeed, so the two compose and never conflict. Default 1
    // (normal). In-play scenes (minigame, battle) override this so a long, active session
    // doesn't starve/sadden the pet; the scaling is uniform, so aging/evolution slow too.
    virtual float careSpeed() const { return 1.0f; }

    // Which music track plays while this scene is up, as a bank id (see sfx.hpp / bank.hpp).
    // Default is the home theme, so a scene only says anything when it differs. App::setScene
    // asks every scene this on the way in, which is why it is a property here rather than a
    // list of scene ids in app.cpp: a new scene brings its own answer, and -- since the bank
    // is data -- a mod-defined track becomes reachable without a firmware change.
    virtual const char* musicId() const { return sfx::kMusicHome; }
};

// Care-sim speed while an "in-play" scene (minigame/battle) is active. 0 = frozen (no
// hunger/health decay, poop, sickness, aging, or energy regen during play); 1 = no change.
// Safe from parking exploits: these scenes are active/timed, so you can't idle in them.
constexpr float IN_PLAY_CARE_SPEED = 0.0f;

// Minimal single-scene manager (swap scenes with set()).
class SceneManager {
    Scene* cur_ = nullptr;
public:
    Scene* current() const { return cur_; }
    void set(Scene* s) { if (cur_) cur_->onExit(); cur_ = s; if (cur_) cur_->onEnter(); }
    void update(float dt) { if (cur_) cur_->update(dt); }
    void render() { if (cur_) cur_->render(); }
    void input(const Input& in) { if (cur_) cur_->onInput(in); }
};
