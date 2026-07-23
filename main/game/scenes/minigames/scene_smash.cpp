#include "scene_smash.hpp"
#include "engine/gfx.hpp"
#include "engine/minigame.hpp"        // shared READY energy line + game-over card
#include "engine/training.hpp"        // grant_training() shared reward gate
#include "core/app.hpp"
#include "assets/sprites.hpp"     // spr_unknown_data (fallback), SPRITE_*
#include "esp_log.h"
#include <cmath>

static const Sprite SPR_FALLBACK { spr_unknown_data, SPRITE_W, SPRITE_H, SPRITE_TRANSP };

// scene geometry
static const int GROUND_Y = 236;
static const int PET_X0   = 60;                 // pet rest x (feet on the ground)
static const int ROCK_CX  = 168, ROCK_CY = 200, ROCK_R = 36;
static const int PB_X = 20, PB_Y = 250, PB_W = 200, PB_H = 26;

// mechanics (tuning knobs; see docs/training-and-energy.md)
static const float SMASH_TIME = 20.0f;   // session length (seconds)
static const float SHAKE_GAIN = 4.0f;    // power per unit of accel MOVEMENT (see below)
static const float DECAY       = 14.0f;  // power bleed when not shaking (per second)
static const float PERFECT_AT  = 85.0f;  // power at/above this -> bonus smash
static const float HIT_ANIM    = 0.32f;  // lunge/recoil/popup duration
static const float PERFECT_MULT = 1.5f;

// reward: Smash mainly trains Strength, with a little Max HP for the exertion.
static const int   SMASH_STR_DIV = 22;
static const int   SMASH_HP_DIV  = 55;
static const float ENERGY_BASE   = 10.0f;
static const float ENERGY_PER    = 0.035f;

void SceneSmash::reset()
{
    phase_ = READY;
    t_ = 0.0f;
    timeLeft_ = SMASH_TIME;
    power_ = 0.0f;
    shakeInst_ = 0.0f;
    ax_ = 0.0f; ay_ = 0.0f; az_ = 1.0f;
    pax_ = 0.0f; pay_ = 0.0f; paz_ = 1.0f;
    score_ = 0;
    tapped_ = false;
    hitT_ = 0.0f;
    lastHitVal_ = 0;
    perfect_ = false;
    ringR_ = 0.0f;
    ringOn_ = false;
    shakeScreen_ = 0.0f;
    rewarded_ = false;
    tired_ = false;
    gainStr_ = gainHp_ = 0;
}

void SceneSmash::strike()
{
    if (power_ < 4.0f) return;                 // too little charge to land a hit (wasted tap)
    perfect_ = power_ >= PERFECT_AT;
    int val = (int)(power_ * (perfect_ ? PERFECT_MULT : 1.0f));
    score_ += val;
    lastHitVal_ = val;
    hitT_ = HIT_ANIM;
    shakeScreen_ = perfect_ ? 9.0f : 5.0f;
    ringR_ = 6.0f; ringOn_ = true;
    power_ = 0.0f;
}

void SceneSmash::award()
{
    if (rewarded_) return;
    rewarded_ = true;

    int   rawStr = score_ / SMASH_STR_DIV;
    int   rawHp  = score_ / SMASH_HP_DIV;
    float cost   = ENERGY_BASE + (float)score_ * ENERGY_PER;

    StatGain gains[] = { { STAT_STR, rawStr }, { STAT_MAXHP, rawHp } };
    TrainingResult r = grant_training(app().pet, cost, gains, 2, 2 + score_ / 120);

    tired_   = r.tired;
    gainStr_ = r.granted[STAT_STR];
    gainHp_  = r.granted[STAT_MAXHP];
    ESP_LOGI("SMASH", "reward: score=%d +%d STR +%d HP (tired=%d spent=%.0f)",
             score_, gainStr_, gainHp_, r.tired ? 1 : 0, r.energySpent);
}

void SceneSmash::onEnter() { reset(); }

void SceneSmash::update(float dt)
{
    t_ += dt;
    bool tap = tapped_; tapped_ = false;

    // fx decay (always)
    if (shakeScreen_ > 0.0f) { shakeScreen_ -= dt * 30.0f; if (shakeScreen_ < 0.0f) shakeScreen_ = 0.0f; }
    if (hitT_ > 0.0f) hitT_ -= dt;
    if (ringOn_) { ringR_ += dt * 440.0f; if (ringR_ > 96.0f) ringOn_ = false; }

    // Shake metric = how much the accel VECTOR moved since last frame. The old approach
    // (instantaneous |mag-1g|) aliased badly against the ~10Hz sensor: whether a frame
    // caught a peak or a zero-crossing of the shake was luck, so the fill felt random.
    // Summing per-sample movement instead measures total motion regardless of sample phase,
    // so vigorous shaking always fills faster. (delta is 0 on frames with no new sample.)
    float dax = ax_ - pax_, day = ay_ - pay_, daz = az_ - paz_;
    float delta = sqrtf(dax * dax + day * day + daz * daz);
    pax_ = ax_; pay_ = ay_; paz_ = az_;
    // jitter viz: a decaying peak of the movement (stays ~0..3 so the pet vibrates while shaking)
    if (delta > shakeInst_) shakeInst_ = delta;
    else                    shakeInst_ = fmaxf(0.0f, shakeInst_ - dt * 4.0f);

    switch (phase_) {
        case READY:
            if (tap) { phase_ = PLAY; timeLeft_ = SMASH_TIME; power_ = 0.0f; score_ = 0; }
            break;

        case PLAY:
            timeLeft_ -= dt;
            power_ += delta * SHAKE_GAIN;          // charge by shaking (per-sample movement)
            power_ -= DECAY * dt;                  // ...bleeding away if you stop
            if (power_ < 0.0f) power_ = 0.0f;
            if (power_ > 100.0f) power_ = 100.0f;
            if (tap) strike();
            if (timeLeft_ <= 0.0f) { timeLeft_ = 0.0f; award(); phase_ = OVER; }
            break;

        case OVER:
            if (tap) app().setScene(SceneId::Activities, Slide::Iris);
            break;
    }
}

void SceneSmash::render()
{
    // screen shake offset (applied to the arena elements)
    int sx = (int)(shakeScreen_ * sinf(t_ * 90.0f));
    int sy = (int)(shakeScreen_ * 0.6f * cosf(t_ * 80.0f));

    fb.fillScreen(rgb565(38, 30, 46));
    fb.fillRect(0, GROUND_Y + sy, GAME_W, GAME_H - GROUND_Y, rgb565(58, 48, 40));   // ground band

    gfx_text(16, 10, 3, col::accent, "SMASH!");

    if (phase_ == READY) {
        gfx_text(20, 60,  1, col::white, "SHAKE the device to charge power,");
        gfx_text(20, 76,  1, col::white, "then TAP to smash the rock.");
        gfx_text(20, 92,  1, col::white, "Power bleeds away - hit near FULL!");
        gfx_text(20, 116, 1, col::dim,   "Trains Strength (STR).");
        mg_energy_readout(20, 140, (int)app().pet.energy());
        gfx_text(48, 206, 2, col::good, "TAP TO START");
        return;
    }

    // --- arena: rock + pet ---
    float hf = (hitT_ > 0.0f) ? (hitT_ / HIT_ANIM) : 0.0f;   // 1 at impact -> 0
    int rockDX = (int)(11.0f * hf);                          // rock recoils right
    int rcx = ROCK_CX + rockDX + sx, rcy = ROCK_CY + sy;
    fb.fillCircle(rcx, rcy, ROCK_R, rgb565(120, 120, 132));
    fb.fillCircle(rcx - 8, rcy - 8, ROCK_R / 3, rgb565(150, 150, 162));   // highlight
    fb.drawCircle(rcx, rcy, ROCK_R, rgb565(70, 70, 80));

    // shockwave ring on impact
    if (ringOn_) fb.drawCircle(rcx, rcy, (int)ringR_, rgb565(255, 220, 120));

    // pet: lunges toward the rock on impact, jitters while charging
    int jitter = (phase_ == PLAY && hitT_ <= 0.0f) ? (int)(shakeInst_ * 4.0f * sinf(t_ * 40.0f)) : 0;
    int petX = PET_X0 + (int)(28.0f * hf) + jitter + sx;
    int feet = GROUND_Y + sy;
    LGFX_Sprite* spr = app().creatures.sprite(app().pet.creatureIndex());
    if (spr) gfx_blit_sprite_bottom(spr, petX, feet, SPRITE_TRANSP);
    else     gfx_blit(SPR_FALLBACK, petX, feet - SPRITE_H / 2);

    // damage popup
    if (hitT_ > 0.0f) {
        int py = rcy - ROCK_R - 12 - (int)((HIT_ANIM - hitT_) * 46.0f);
        gfx_text(rcx - 14, py, 2, perfect_ ? rgb565(255, 220, 90) : col::white,
                 "%d%s", lastHitVal_, perfect_ ? "!" : "");
    }

    // --- power bar ---
    fb.fillRoundRect(PB_X, PB_Y, PB_W, PB_H, 6, rgb565(30, 30, 40));
    int fillW = (int)(PB_W * power_ / 100.0f);
    uint16_t pcol = (power_ >= PERFECT_AT) ? rgb565(255, 215, 90)
                  : (power_ >= 50.0f)      ? rgb565(240, 200, 70)
                                           : rgb565(90, 210, 110);
    if (fillW > 0) fb.fillRoundRect(PB_X, PB_Y, fillW < 6 ? 6 : fillW, PB_H, 6, pcol);
    int mk = PB_X + (int)(PB_W * PERFECT_AT / 100.0f);           // perfect-zone marker
    fb.fillRect(mk, PB_Y - 3, 2, PB_H + 6, rgb565(255, 240, 150));
    fb.drawRoundRect(PB_X, PB_Y, PB_W, PB_H, 6, col::dim);

    // HUD: timer + prompt
    gfx_text(GAME_W - 52, 14, 2, timeLeft_ < 5.0f ? col::warn : col::white, "%d", (int)ceilf(timeLeft_));
    if (phase_ == PLAY) {
        gfx_text(20, 288, 1, col::dim, "SHAKE to charge  -  TAP to smash!");
        gfx_text(20, 302, 1, col::accent, "Score %d", score_);
    }

    if (phase_ == OVER) {
        mg_over_card("SMASHED!");
        mg_center(MG_CARD_Y + 40, 2, col::white, "Score %d", score_);
        mg_center(MG_CARD_Y + 68, 1, tired_ ? col::warn : col::good,
                  "+%d STR  +%d HP%s", gainStr_, gainHp_, tired_ ? "  tired!" : "");
    }
}

void SceneSmash::onInput(const Input& in)
{
    // capture the latest motion snapshot every frame (onInput runs each loop iteration)
    ax_ = in.ax; ay_ = in.ay; az_ = in.az;
    if (in.pressed) tapped_ = true;
}
