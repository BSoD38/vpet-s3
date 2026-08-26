#include "scene_stance.hpp"
#include "minigame.hpp"        // shared READY energy line + game-over card
#include "engine/gfx.hpp"
#include "sim/training.hpp"        // grant_training() shared reward gate
#include "core/app.hpp"
#include "assets/sprites.hpp"     // spr_unknown_data (fallback), SPRITE_*
#include "esp_random.h"
#include "esp_log.h"
#include <cmath>

static const Sprite SPR_FALLBACK { spr_unknown_data, SPRITE_W, SPRITE_H, SPRITE_TRANSP };

// ---- tilt calibration -------------------------------------------------------------------
// The roll axis (which accel axis swings with left/right tilt) is AUTO-DETECTED at round
// start: whichever of X/Y reads closest to 0 at rest is the screen-horizontal one that
// moves with roll (the other reads ~1g = "down" and responds the SAME way to either tilt,
// which is unusable). Only the sign is a manual knob: flip TILT_SIGN if left/right is mirrored.
static const float TILT_SIGN  = 1.0f;   // flip to -1 if tilting one way moves the pet the wrong way
static const float TILT_RANGE = 0.35f;  // g of tilt for full control input (~20 deg -> quite responsive)

// ---- balance dynamics (tuning knobs) ----------------------------------------------------
static const float CONTROL = 2.8f;      // how hard your tilt corrects
static const float INSTAB  = 1.0f;      // unstable drift: pushes further the more off-center
static const float DAMP    = 1.6f;      // velocity friction
static const float GUST_INT0     = 2.4f, GUST_INT_FLOOR = 0.85f, GUST_INT_DEC = 0.03f;
static const float GUST_MAG0     = 0.5f, GUST_MAG_INC   = 0.015f, GUST_MAG_CAP = 1.3f;

// ---- reward: mainly Endurance, plus Max HP (holding out = stamina) -----------------------
static const float END_PER     = 1.2f;  // END per second survived
static const float HP_PER      = 0.6f;  // Max HP per second survived
static const float ENERGY_BASE = 8.0f;
static const float ENERGY_PER  = 0.6f;

// scene geometry
static const int CX = 120, CY = 182, HL = 96, TH = 9;   // beam center, half-length, half-thickness

static float rnd01() { return (float)(esp_random() & 0xFFFF) / 65535.0f; }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

void SceneStance::reset()
{
    phase_ = READY;
    t_ = 0.0f;
    survived_ = 0.0f;
    bal_ = 0.0f;
    v_ = 0.0f;
    neutralA_ = 0.0f;
    tiltInput_ = 0.0f;
    ax_ = 0.0f; ay_ = 0.0f; az_ = 1.0f;
    gustT_ = 0.0f;
    gustFlash_ = 0.0f;
    gustDir_ = 1;
    tapped_ = false;
    rewarded_ = false;
    tired_ = false;
    gainEnd_ = gainHp_ = 0;
}

float SceneStance::tiltAxis() const { return rollAxis_ == 0 ? ax_ : ay_; }

float SceneStance::gustInterval() const
{
    float v = GUST_INT0 - survived_ * GUST_INT_DEC;
    return v < GUST_INT_FLOOR ? GUST_INT_FLOOR : v;
}

void SceneStance::startPlay()
{
    phase_ = PLAY;
    survived_ = 0.0f;
    bal_ = 0.0f;
    v_ = 0.0f;
    // Auto-pick the roll axis: the in-plane axis freest at rest (smallest |g|) is the
    // screen-horizontal one that swings with left/right tilt. The other reads ~1g ("down")
    // and moves the same way for either tilt -> useless for roll. This fixes the "both
    // directions tilt the platform the same way" bug without hardcoding a board axis.
    rollAxis_ = (fabsf(ax_) <= fabsf(ay_)) ? 0 : 1;
    neutralA_ = tiltAxis();          // capture the resting orientation as center
    gustT_ = GUST_INT0;
}

void SceneStance::award()
{
    if (rewarded_) return;
    rewarded_ = true;

    int   rawEnd = (int)(survived_ * END_PER);
    int   rawHp  = (int)(survived_ * HP_PER);
    float cost   = ENERGY_BASE + survived_ * ENERGY_PER;

    StatGain gains[] = { { STAT_END, rawEnd }, { STAT_MAXHP, rawHp } };
    TrainingResult r = grant_training(app().pet, app().economy, cost, gains, 2, 2 + (int)survived_ / 8);
    bits_ = r.bits;

    // Stillness and control. Validated as a set by tools/personality_sim.py.
    static const float DRIFT_STANCE[AX_COUNT] = { 0.00f, -1.00f, -0.30f, -0.60f };
    app().pet.nudgeDrift(DRIFT_STANCE);

    tired_   = r.tired;
    gainEnd_ = r.granted[STAT_END];
    gainHp_  = r.granted[STAT_MAXHP];
    ESP_LOGI("STANCE", "reward: time=%.1fs +%d END +%d HP (tired=%d spent=%.0f)",
             survived_, gainEnd_, gainHp_, r.tired ? 1 : 0, r.energySpent);
}

void SceneStance::onEnter() { reset(); }

void SceneStance::update(float dt)
{
    t_ += dt;
    bool tap = tapped_; tapped_ = false;
    if (gustFlash_ > 0.0f) gustFlash_ -= dt;

    // tilt control (relative to the neutral captured at start)
    float raw = (tiltAxis() - neutralA_) * TILT_SIGN / TILT_RANGE;
    float target = clampf(raw, -1.0f, 1.0f);
    tiltInput_ += (target - tiltInput_) * fminf(1.0f, dt * 16.0f);   // light smoothing (snappy)

    switch (phase_) {
        case READY:
            if (tap) startPlay();
            break;

        case PLAY: {
            survived_ += dt;

            // gusts: random shoves that grow with time
            gustT_ -= dt;
            if (gustT_ <= 0.0f) {
                gustDir_ = (esp_random() & 1) ? 1 : -1;
                float mag = GUST_MAG0 + survived_ * GUST_MAG_INC;
                if (mag > GUST_MAG_CAP) mag = GUST_MAG_CAP;
                v_ += gustDir_ * mag;
                gustFlash_ = 0.35f;
                gustT_ = gustInterval();
            }

            // integrate: your tilt corrects, instability fights back, friction damps
            float accel = CONTROL * tiltInput_ + INSTAB * bal_;
            v_ += accel * dt;
            v_ -= v_ * clampf(DAMP * dt, 0.0f, 1.0f);
            bal_ += v_ * dt;

            if (bal_ <= -1.0f || bal_ >= 1.0f) { bal_ = clampf(bal_, -1.0f, 1.0f); award(); phase_ = OVER; }
            break;
        }

        case OVER:
            if (tap) app().setScene(SceneId::Activities, Slide::Iris);
            break;
    }
}

void SceneStance::render()
{
    fb.fillScreen(rgb565(24, 34, 46));
    gfx_text(16, 10, 3, col::accent, "STANCE");

    if (phase_ == READY) {
        gfx_text(20, 60,  1, col::white, "Balance on the beam by");
        gfx_text(20, 76,  1, col::white, "TILTING the device left/right.");
        gfx_text(20, 92,  1, col::white, "Gusts push you - keep centered!");
        gfx_text(20, 116, 1, col::dim,   "Trains Endurance (END) + Max HP.");
        gfx_text(20, 132, 1, col::dim,   "Hold it comfortably, then start.");
        mg_energy_readout(20, 156, (int)app().pet.energy());
        gfx_text(48, 210, 2, col::good, "TAP TO START");
        return;
    }

    // survival timer
    gfx_text(GAME_W - 92, 16, 2, col::white, "%.1fs", survived_);

    // balance indicator: fills from the CENTER toward the side you're leaning, reddening
    // near the edge. Empty = perfectly centered = good (that's the goal), so it reads clearly.
    int BX = 20, BW = 200, BY = 52, BH = 12, cxb = BX + BW / 2;
    gfx_text(BX, BY - 12, 1, col::dim, "BALANCE");
    fb.fillRoundRect(BX, BY, BW, BH, 5, rgb565(38, 42, 54));
    float ab = fabsf(bal_);
    uint16_t fc = ab > 0.7f ? rgb565(240, 80, 70) : ab > 0.4f ? rgb565(240, 200, 80) : rgb565(90, 210, 120);
    int fillw = (int)(ab * (BW / 2 - 3));
    if (fillw > 0) fb.fillRoundRect(bal_ >= 0 ? cxb : cxb - fillw, BY + 2, fillw, BH - 4, 3, fc);
    fb.fillRect(cxb - 1, BY - 2, 2, BH + 4, col::white);   // center reference

    // the beam (rotated by the current tilt) - an anti-aliased thick line, so its edges stay
    // smooth at any angle instead of the jagged raw-triangle look. r = TH (half-thickness).
    float ang = tiltInput_ * 0.34f;
    float ca = cosf(ang), sa = sinf(ang);
    int Lx = CX - (int)(HL * ca), Ly = CY - (int)(HL * sa);
    int Rx = CX + (int)(HL * ca), Ry = CY + (int)(HL * sa);
    fb.drawWideLine(Lx, Ly, Rx, Ry, (float)TH, rgb565(150, 110, 70));
    // center pivot marker
    fb.fillSmoothCircle(CX, CY, 4, rgb565(90, 200, 120));

    // pet standing on the beam at its balance position
    float d = bal_ * (HL - 12);
    int feetX = CX + (int)(d * ca), feetY = CY + (int)(d * sa) - TH - 2;
    LGFX_Sprite* spr = app().creatures.sprite(app().pet.creatureIndex());
    if (spr) gfx_blit_sprite_fit_bottom(spr, feetX, feetY, 96, 96, SPRITE_TRANSP);
    else     gfx_blit(SPR_FALLBACK, feetX, feetY - SPRITE_H / 2);

    // gust swoosh from the push side
    if (gustFlash_ > 0.0f) {
        int gy = CY - 40;
        if (gustDir_ > 0) { gfx_text(6,  gy, 2, rgb565(180, 220, 255), ">>>"); }   // pushed right
        else              { gfx_text(GAME_W - 42, gy, 2, rgb565(180, 220, 255), "<<<"); }
    }

    gfx_text(20, 300, 1, col::dim, "Tilt to stay centered");

    // motion debug (toggle "Debug info" in Settings): live accel + auto-picked roll axis,
    // so tilt calibration (e.g. whether to flip TILT_SIGN) can be read off the screen.
    if (app().debugOverlay)
        gfx_text(4, 312, 1, col::dim, "ax%.2f ay%.2f az%.2f roll%d in%.2f",
                 ax_, ay_, az_, rollAxis_, tiltInput_);

    if (phase_ == OVER) {
        mg_over_card("FELL!", bits_);
        mg_center(MG_CARD_Y + 40, 2, col::white, "Time %.1fs", survived_);
        mg_center(MG_CARD_Y + 68, 1, tired_ ? col::warn : col::good,
                  "+%d END  +%d HP%s", gainEnd_, gainHp_, tired_ ? "  tired!" : "");
    }
}

void SceneStance::onInput(const Input& in)
{
    // capture the latest motion snapshot every frame (onInput runs each loop iteration)
    ax_ = in.ax; ay_ = in.ay; az_ = in.az;
    if (in.pressed) tapped_ = true;
}
