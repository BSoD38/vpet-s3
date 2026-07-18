#pragma once
#include "engine/input.hpp"

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
};

// Minimal single-scene manager (swap scenes with set()).
class SceneManager {
    Scene* cur_ = nullptr;
public:
    void set(Scene* s) { if (cur_) cur_->onExit(); cur_ = s; if (cur_) cur_->onEnter(); }
    void update(float dt) { if (cur_) cur_->update(dt); }
    void render() { if (cur_) cur_->render(); }
    void input(const Input& in) { if (cur_) cur_->onInput(in); }
};
