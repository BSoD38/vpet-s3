#pragma once
#include "core/scene.hpp"
#include "ui/widgets.hpp"          // Rect (speech-bubble hit target)
#include "toy_ball.hpp"            // the throwable ball (toys with play: "toss")
#include "engine/anim.hpp"         // CreatureAnim (sprite-frame state machine)
#include "engine/walk.hpp"         // CreatureWalk (ground locomotion, step-locked)
#include "engine/pinch.hpp"        // PinchZoom + Camera2D (pinch-to-zoom over the scene)

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
    float   toyPlayT_ = 0.0f;      // >0 = the out toy is bouncing from a play tap
    ToyBall ball_;                 // live only while the out toy is a throwable one
    bool    ballReady_ = false;    // ball_ has been placed for the toy currently out
    float   ballRewardCd_ = 0.0f;  // metering: bond/drift for play lands on a cooldown, not per bat
    float   ringHold_ = 0.0f;      // >0 = rub-progress ring still drawn (see RING_LINGER)
    float   pokeCd_ = 0.0f;        // short cooldown between individual pokes
    int     pokeCount_ = 0;        // pokes since the last happiness reward
    int     pokeTarget_ = 0;       // pokes needed this cycle (random 3..6; 0 = pick lazily)
    bool    tapConsumed_ = false;  // this press was taken by a HUD control (see onInput)
    bool    overPet_ = false;      // debug: finger over the pet zone this frame
    float   lastMove_ = 0.0f;      // debug: last per-frame movement (px)
    // Speech-bubble hit target. Computed in render() because it tracks the sprite's height;
    // zero-width when no conversation is waiting.
    Rect    bubble_{ 0, 0, 0, 0 };
    CreatureAnim anim_;            // which sheet frame the pet is showing (16-frame creatures)
    CreatureWalk walk_;            // where along the ground it is; travels in step with anim_
                                   // (scene-lived, so it stays put across a menu round-trip)
    // Pinch-to-zoom camera (docs/camera.md). The world is the classic 240x320 scene; zoom
    // shows a sub-rect of it, and the camera GLIDES AFTER THE CREATURE (Camera2D::follow
    // in update()), so the pinch only sets the zoom level. Scene-lived like walk_, so a
    // zoomed-in view survives a menu round-trip; the pinch state itself resets on entry
    // (a gesture can't span scenes).
    Camera2D cam_{ GAME_W, GAME_H, GAME_W, GAME_H };
    PinchZoom pinch_;              // two-finger zoom driving cam_ (zoomOnly; follow pans)
    Input    in_{};                // full per-frame input snapshot (stashed for pinch_)
public:
    // Clear the WHOLE touch snapshot, not just in_: down_/tx_/ty_/wasDown_ are a second
    // copy of it, and leaving them stale from the previous visit let update() compute
    // press edges from one visit's touch against another's.
    void onEnter() override {
        pinch_.reset(); pinch_.zoomOnly = true;
        in_ = Input{}; down_ = false; wasDown_ = false; tx_ = 0; ty_ = 0;
        tapConsumed_ = false;
    }
    void update(float dt) override;
    void render() override;
    void onInput(const Input& in) override;
};
