#pragma once
#include "core/scene.hpp"
#include <cstdint>

// Bulwark (END trainer) - a reactive guard drill. Attacks fly in from the LEFT, TOP, or
// RIGHT toward your pet; tap the matching guard button before each one lands. Correct guard
// = blocked; wrong button or too slow = you take the hit and lose a heart. Attacks speed up
// as your streak grows; lose 3 hearts and it's over. Blocks train Endurance (defense) plus
// a little Agility (reaction). Touch-only.
class SceneBulwark : public Scene {
    enum Phase { READY, PLAY, OVER };
    static const int LIVES_START = 3;

    Phase phase_ = READY;
    float t_ = 0.0f;              // animation clock
    int   score_ = 0;            // successful blocks
    int   lives_ = LIVES_START;

    // incoming attacks. Early on only one is ever in flight; at higher scores several
    // overlap (see maxConcurrent), so you may have to block two or three at once.
    static const int MAX_ATK = 4;
    struct Atk { bool active; int dir; float p; float travel; };   // dir 0=L 1=top 2=R; p 0..1
    Atk   atk_[MAX_ATK] = {};
    float spawnT_ = 0.0f;        // countdown to the next spawn (frozen while at concurrency cap)

    // feedback
    float flashT_ = 0.0f;        // successful-block flash timer
    int   blockDir_ = 0;         // side that was just blocked
    float hurtT_ = 0.0f;         // took-a-hit flash timer

    // input edge (consumed each update)
    bool  tapped_ = false;
    int   pendingBtn_ = -1;      // guard button under the last tap (-1 = none)

    // reward
    bool  rewarded_ = false;
    bool  tired_ = false;
    uint32_t bits_ = 0;              // Bits paid for the session (shown on the card)
    int   gainEnd_ = 0, gainAgi_ = 0;

    void  reset();
    void  startPlay();
    void  spawnAttack();
    void  takeHit();
    void  award();
    float curTravel() const;
    float curGap() const;
    int   activeCount() const;
    int   maxConcurrent() const;      // how many attacks may share the screen at the current score
    static int btnAt(int x, int y);   // guard column under a point (0/1/2)
public:
    void onEnter() override;
    void update(float dt) override;
    void render() override;
    void onInput(const Input& in) override;
    bool allowsSleep() const override { return false; }   // never nap mid-game
    float careSpeed() const override { return IN_PLAY_CARE_SPEED; }  // care frozen while playing
};
