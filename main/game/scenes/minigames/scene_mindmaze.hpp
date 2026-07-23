#pragma once
#include "core/scene.hpp"
#include <cstdint>

// Mind Maze (INT trainer) - a Simon-style memory game. Watch a growing sequence of lit
// pads, then repeat it by tapping. Each cleared round adds one step; a wrong tap ends the
// run. The number of rounds cleared trains Intellect (pure single-stat trainer). Touch-only.
class SceneMindMaze : public Scene {
    enum Phase { READY, SHOW, INPUT, GOOD, OVER };
    static const int MAX_SEQ = 40;      // sequence cap (unreachably long for a memory game)

    Phase   phase_ = READY;
    float   t_ = 0.0f;                  // animation clock
    uint8_t seq_[MAX_SEQ];              // the pattern (each 0..3 = pad index)
    int     len_ = 0;                   // current sequence length (== round number)
    int     playIdx_ = 0;               // playback cursor during SHOW
    int     inputIdx_ = 0;              // how many pads the player has correctly echoed
    int     completed_ = 0;             // rounds fully repeated (the score)
    int     lit_ = -1;                  // currently highlighted pad (-1 = none)
    bool    showOn_ = false;            // SHOW: pad currently lit (vs. inter-step gap)
    bool    wrong_ = false;             // last input was wrong (flash the mistaken pad red)
    float   pt_ = 0.0f;                 // phase timer (playback steps / success pause)
    float   flashT_ = 0.0f;             // input-tap highlight decay

    bool    tapped_ = false;            // input edge, consumed each update
    int     tapPad_ = -1;               // pad under the last tap (-1 = none / off-pad)

    bool    rewarded_ = false;
    bool    tired_ = false;
    int     gainInt_ = 0;

    void reset();
    void nextRound();                   // append a random step and start playback
    void startShow();
    void award();
    static int padAt(int x, int y);     // pad index under a point, or -1
public:
    void onEnter() override;
    void update(float dt) override;
    void render() override;
    void onInput(const Input& in) override;
    bool allowsSleep() const override { return false; }   // never nap mid-game
    float careSpeed() const override { return IN_PLAY_CARE_SPEED; }  // care frozen while playing
};
