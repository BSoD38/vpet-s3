#include "scene_mindmaze.hpp"
#include "minigame.hpp"        // shared READY energy line + game-over card
#include "engine/gfx.hpp"
#include "sim/training.hpp"     // grant_training() shared reward gate
#include "core/app.hpp"
#include "assets/sprites.hpp"  // spr_unknown_data (fallback), SPRITE_*
#include "esp_random.h"
#include "esp_log.h"
#include <cstring>

static const Sprite SPR_FALLBACK { spr_unknown_data, SPRITE_W, SPRITE_H, SPRITE_TRANSP };

// 2x2 pad grid (classic Simon layout). Geometry is centered on the 240-wide panel; the grid
// is sized to leave a header band up top for the title/prompt and the pet.
static const int PAD_W = 80, PAD_H = 80, PAD_G = 12;
static const int GRID_X = (GAME_W - (PAD_W * 2 + PAD_G)) / 2;   // 34
static const int GRID_Y = 120;
static const int PET_CAP = 64;   // pet sprite draw cap (big sprites scaled down)

static const uint16_t PAD_DIM[4] = {
    rgb565(120, 40, 40), rgb565(40, 110, 50), rgb565(45, 60, 130), rgb565(130, 115, 35),
};
static const uint16_t PAD_LIT[4] = {
    rgb565(255, 95, 95), rgb565(120, 245, 130), rgb565(120, 160, 255), rgb565(255, 232, 95),
};

// Playback / feedback timing (seconds).
static const float SHOW_ON    = 0.42f;   // a pad stays lit this long during playback
static const float SHOW_OFF   = 0.16f;   // dark gap between playback steps
static const float FLASH_DUR  = 0.22f;   // how long a tapped pad stays highlighted
static const float GOOD_PAUSE = 0.55f;   // beat after a cleared round before the next playback

// Reward tuning (knobs; see docs/training-and-energy.md). Mind Maze is a pure INT trainer:
// gains scale with rounds cleared, and a session costs stamina scaling the same way.
static const int   INT_PER_RND    = 4;
static const float ENERGY_BASE    = 6.0f;
static const float ENERGY_PER_RND = 2.2f;

void SceneMindMaze::reset()
{
    phase_ = READY;
    t_ = 0.0f;
    len_ = playIdx_ = inputIdx_ = completed_ = 0;
    lit_ = -1;
    showOn_ = false;
    wrong_ = false;
    pt_ = flashT_ = 0.0f;
    tapped_ = false;
    tapPad_ = -1;
    rewarded_ = false;
    tired_ = false;
    gainInt_ = 0;
}

void SceneMindMaze::startShow()
{
    phase_   = SHOW;
    playIdx_ = 0;
    showOn_  = true;
    lit_     = seq_[0];
    pt_      = SHOW_ON;
}

void SceneMindMaze::nextRound()
{
    if (len_ < MAX_SEQ) seq_[len_++] = (uint8_t)(esp_random() % 4);
    startShow();
}

void SceneMindMaze::award()
{
    if (rewarded_) return;
    rewarded_ = true;

    int   rawInt = completed_ * INT_PER_RND;
    float cost   = ENERGY_BASE + (float)completed_ * ENERGY_PER_RND;

    StatGain gains[] = { { STAT_INT, rawInt } };
    TrainingResult r = grant_training(app().pet, app().economy, cost, gains, 1, 2 + completed_ / 4);
    bits_ = r.bits;

    // Quiet, cerebral, self-directed -- and the main NON-neglectful route to a timid,
    // independent temperament. Validated as a set by tools/personality_sim.py.
    static const float DRIFT_MAZE[AX_COUNT] = { -0.30f, -0.60f, -0.30f, -0.80f };
    app().pet.nudgeDrift(DRIFT_MAZE);

    tired_   = r.tired;
    gainInt_ = r.granted[STAT_INT];
    ESP_LOGI("MAZE", "reward: rounds=%d +%d INT (tired=%d spent=%.0f)",
             completed_, gainInt_, r.tired ? 1 : 0, r.energySpent);
}

int SceneMindMaze::padAt(int x, int y)
{
    for (int i = 0; i < 4; i++) {
        int px = GRID_X + (i % 2) * (PAD_W + PAD_G);
        int py = GRID_Y + (i / 2) * (PAD_H + PAD_G);
        if (x >= px && x < px + PAD_W && y >= py && y < py + PAD_H) return i;
    }
    return -1;
}

void SceneMindMaze::onEnter() { reset(); }

void SceneMindMaze::update(float dt)
{
    t_ += dt;
    bool tap = tapped_; tapped_ = false;
    int  pad = tapPad_; tapPad_ = -1;

    // decay a tapped-pad highlight (only during INPUT; SHOW manages lit_ itself)
    if (flashT_ > 0.0f) { flashT_ -= dt; if (flashT_ <= 0.0f && phase_ == INPUT) lit_ = -1; }

    switch (phase_) {
        case READY:
            if (tap) nextRound();               // any tap starts round 1
            break;

        case SHOW:
            pt_ -= dt;
            if (pt_ <= 0.0f) {
                if (showOn_) {                  // finished lighting a pad -> dark gap
                    showOn_ = false; lit_ = -1; pt_ = SHOW_OFF;
                } else {                        // gap done -> advance to the next pad
                    playIdx_++;
                    if (playIdx_ >= len_) { phase_ = INPUT; inputIdx_ = 0; lit_ = -1; }
                    else { showOn_ = true; lit_ = seq_[playIdx_]; pt_ = SHOW_ON; }
                }
            }
            break;

        case INPUT:
            if (tap && pad >= 0) {
                lit_ = pad; flashT_ = FLASH_DUR;         // highlight the tapped pad
                if (pad == seq_[inputIdx_]) {            // correct so far
                    if (++inputIdx_ >= len_) { completed_ = len_; phase_ = GOOD; pt_ = GOOD_PAUSE; }
                } else {                                 // wrong -> end the run
                    wrong_ = true;
                    award();
                    phase_ = OVER;
                }
            }
            break;

        case GOOD:
            pt_ -= dt;
            if (pt_ <= 0.0f) nextRound();
            break;

        case OVER:
            if (tap) app().setScene(SceneId::Activities, Slide::Iris);
            break;
    }
}

void SceneMindMaze::render()
{
    fb.fillScreen(col::panel);
    gfx_text(28, 10, 2, col::accent, "MIND MAZE");

    if (phase_ == READY) {
        gfx_text(24, 60,  1, col::white, "Watch the pads light up,");
        gfx_text(24, 76,  1, col::white, "then tap them in the same order.");
        gfx_text(24, 92,  1, col::white, "Each round adds one more step.");
        gfx_text(24, 116, 1, col::dim,   "Trains Intellect (INT).");
        mg_energy_readout(24, 140, (int)app().pet.energy());
        gfx_text(48, 210, 2, col::good, "TAP TO START");
        return;
    }

    // round indicator
    gfx_text(GAME_W - 84, 12, 1, col::dim, "Round %d", len_);

    // phase prompt (top, under the title)
    if (phase_ != OVER) {
        const char* msg = (phase_ == SHOW) ? "WATCH" : (phase_ == INPUT) ? "YOUR TURN" : "NICE!";
        uint16_t mc     = (phase_ == SHOW) ? col::warn : (phase_ == INPUT) ? col::good : col::accent;
        int w = (int)strlen(msg) * 12;
        gfx_text((GAME_W - w) / 2, 32, 2, mc, "%s", msg);
    }

    // the pet, watching along (big sprites capped), standing just above the grid
    LGFX_Sprite* spr = app().creatures.sprite(app().pet.creatureIndex());
    if (spr) gfx_blit_sprite_fit_bottom(spr, GAME_W / 2, GRID_Y - 8, PET_CAP, PET_CAP, SPRITE_TRANSP);
    else     gfx_blit(SPR_FALLBACK, GAME_W / 2, GRID_Y - 8 - SPRITE_H / 2);

    // pads
    for (int i = 0; i < 4; i++) {
        int px = GRID_X + (i % 2) * (PAD_W + PAD_G);
        int py = GRID_Y + (i / 2) * (PAD_H + PAD_G);
        bool on = (lit_ == i);
        uint16_t c = on ? PAD_LIT[i] : PAD_DIM[i];
        if (on && wrong_ && phase_ == OVER) c = rgb565(255, 55, 55);   // the mistaken pad
        fb.fillRoundRect(px, py, PAD_W, PAD_H, 12, c);
        fb.drawRoundRect(px, py, PAD_W, PAD_H, 12, on ? col::white : rgb565(30, 32, 40));
    }

    if (phase_ == OVER) {
        mg_over_card("MEMORY!", bits_);
        mg_center(MG_CARD_Y + 40, 2, col::white, "Rounds %d", completed_);
        mg_center(MG_CARD_Y + 68, 1, tired_ ? col::warn : col::good,
                  "+%d INT%s", gainInt_, tired_ ? "  tired!" : "");
    }
}

void SceneMindMaze::onInput(const Input& in)
{
    if (!in.pressed) return;
    tapped_ = true;
    tapPad_ = padAt(in.x, in.y);
}
