#pragma once
#include "core/scene.hpp"

// Home / care screen: shows the pet, a HUD (stats + battery + clock), a MENU button,
// and lets you pet the creature by rubbing a finger over it (raises happiness).
class SceneHome : public Scene {
    float   t_ = 0.0f;             // animation time
    bool    down_ = false;         // latest touch state (from onInput)
    int16_t tx_ = 0, ty_ = 0;      // latest touch position
    int16_t prevx_ = 0, prevy_ = 0;
    bool    wasDown_ = false;
    // gesture tracking for the current touch that started on the creature
    bool    touchActive_ = false;  // this touch began over the pet
    float   touchDur_ = 0.0f;      // how long it has been held
    float   rubDist_ = 0.0f;       // total movement since press
    float   rubProgress_ = 0.0f;   // accumulated active-rub time toward next chunk
    float   hopTimer_ = 0.0f;      // >0 = quick poke hop in progress (every poke)
    float   sparkTimer_ = 0.0f;    // >0 = poke-reward sparkles lingering
    float   playCooldown_ = 0.0f;  // >0 = petting locked out; hearts linger this long
    float   refuseTimer_ = 0.0f;   // >0 = pet is doing a "no" head-shake (refused an action)
    float   pokeCd_ = 0.0f;        // short cooldown between individual pokes
    int     pokeCount_ = 0;        // pokes since the last happiness reward
    int     pokeTarget_ = 0;       // pokes needed this cycle (random 3..6; 0 = pick lazily)
    bool    rubbingNow_ = false;   // finger over the pet and moving this frame
    bool    overPet_ = false;      // debug: finger over the pet zone this frame
    float   lastMove_ = 0.0f;      // debug: last per-frame movement (px)
public:
    void update(float dt) override;
    void render() override;
    void onInput(const Input& in) override;
};
