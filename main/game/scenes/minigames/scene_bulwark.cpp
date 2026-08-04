#include "scene_bulwark.hpp"
#include "minigame.hpp"        // shared READY energy line + game-over card
#include "engine/gfx.hpp"
#include "sim/training.hpp"        // grant_training() shared reward gate
#include "core/app.hpp"
#include "assets/sprites.hpp"     // spr_unknown_data (fallback), SPRITE_*
#include "esp_random.h"
#include "esp_log.h"
#include <cmath>

static const Sprite SPR_FALLBACK { spr_unknown_data, SPRITE_W, SPRITE_H, SPRITE_TRANSP };

// scene geometry. The three guards map to the LEFT / MIDDLE / RIGHT THIRDS of the whole
// screen (not just the little buttons) so a flat-thumb tap anywhere in a column registers
// -- precision doesn't matter, which is what a fast reaction game needs.
static const int PET_CX = 120, PET_CY = 150;
static const int COL_W   = GAME_W / 3;            // tap-column width (80)
static const int GUARD_Y = 246, GUARD_H = 64;     // visual guard buttons (fill their column)
static const int GUARD_M = 5;                      // inset within the column

// difficulty ramp (tuning knobs)
static const float TRAVEL0     = 1.60f;   // spawn->pet time at score 0
static const float TRAVEL_FLOOR = 0.62f;  // ...tightening to this
static const float TRAVEL_DEC  = 0.030f;  // per successful block
static const float GAP0        = 0.75f;   // pause between attacks at score 0
static const float GAP_FLOOR   = 0.32f;
static const float GAP_DEC     = 0.012f;
static const float FIRST_GAP   = 0.80f;
static const float BLOCK_FLASH = 0.22f;
static const float HURT_FLASH  = 0.32f;

// reward: Bulwark mainly trains Endurance (defense), plus a little Agility (reaction).
static const int   END_PER     = 4;
static const int   END_DIV     = 5;
static const int   AGI_DIV     = 6;       // gainAgi = score / AGI_DIV
static const float ENERGY_BASE = 8.0f;
static const float ENERGY_PER  = 0.5f;

void SceneBulwark::reset()
{
    phase_ = READY;
    t_ = 0.0f;
    score_ = 0;
    lives_ = LIVES_START;
    for (int i = 0; i < MAX_ATK; i++) atk_[i].active = false;
    spawnT_ = 0.0f;
    flashT_ = 0.0f;
    blockDir_ = 0;
    hurtT_ = 0.0f;
    tapped_ = false;
    pendingBtn_ = -1;
    rewarded_ = false;
    tired_ = false;
    gainEnd_ = gainAgi_ = 0;
}

float SceneBulwark::curTravel() const
{
    float v = TRAVEL0 - (float)score_ * TRAVEL_DEC;
    return v < TRAVEL_FLOOR ? TRAVEL_FLOOR : v;
}
float SceneBulwark::curGap() const
{
    float v = GAP0 - (float)score_ * GAP_DEC;
    return v < GAP_FLOOR ? GAP_FLOOR : v;
}

int SceneBulwark::activeCount() const
{
    int n = 0;
    for (int i = 0; i < MAX_ATK; i++) if (atk_[i].active) n++;
    return n;
}

// One attack in flight below score 70, then two, then three -- so high scores force you to
// track and block multiple threats at once instead of one-at-a-time.
int SceneBulwark::maxConcurrent() const
{
    return score_ >= 110 ? 3 : score_ >= 70 ? 2 : 1;
}

void SceneBulwark::startPlay()
{
    phase_ = PLAY;
    score_ = 0;
    lives_ = LIVES_START;
    for (int i = 0; i < MAX_ATK; i++) atk_[i].active = false;
    spawnT_ = FIRST_GAP;
}

void SceneBulwark::spawnAttack()
{
    for (int i = 0; i < MAX_ATK; i++) {
        if (!atk_[i].active) {
            atk_[i].active = true;
            atk_[i].dir    = (int)(esp_random() % 3);
            atk_[i].p      = 0.0f;
            atk_[i].travel = curTravel();
            return;
        }
    }
}

void SceneBulwark::takeHit()
{
    lives_--;
    hurtT_ = HURT_FLASH;
}

void SceneBulwark::award()
{
    if (rewarded_) return;
    rewarded_ = true;

    int   rawEnd = score_ * END_PER / END_DIV;
    int   rawAgi = score_ / AGI_DIV;
    float cost   = ENERGY_BASE + (float)score_ * ENERGY_PER;

    StatGain gains[] = { { STAT_END, rawEnd }, { STAT_AGI, rawAgi } };
    TrainingResult r = grant_training(app().pet, cost, gains, 2, 2 + score_ / 6);

    // Disciplined bravery -- the deliberate brave+/wild- decorrelator. Without a source
    // like this, "brave" and "unruly" would always move together and a whole quadrant of
    // personalities would be unreachable. Validated by tools/personality_sim.py.
    static const float DRIFT_BULWARK[AX_COUNT] = { 1.00f, 0.60f, -0.30f, -1.00f };
    app().pet.nudgeDrift(DRIFT_BULWARK);

    tired_   = r.tired;
    gainEnd_ = r.granted[STAT_END];
    gainAgi_ = r.granted[STAT_AGI];
    ESP_LOGI("BULWARK", "reward: blocks=%d +%d END +%d AGI (tired=%d spent=%.0f)",
             score_, gainEnd_, gainAgi_, r.tired ? 1 : 0, r.energySpent);
}

int SceneBulwark::btnAt(int x, int y)
{
    (void)y;                              // whole screen is live: only the column matters
    if (x < COL_W)     return 0;          // left third
    if (x < 2 * COL_W) return 1;          // middle third
    return 2;                             // right third
}

void SceneBulwark::onEnter() { reset(); }

void SceneBulwark::update(float dt)
{
    t_ += dt;
    bool tap = tapped_; tapped_ = false;
    int  btn = pendingBtn_; pendingBtn_ = -1;

    if (flashT_ > 0.0f) flashT_ -= dt;
    if (hurtT_  > 0.0f) hurtT_  -= dt;

    switch (phase_) {
        case READY:
            if (tap) startPlay();
            break;

        case PLAY: {
            // 1) a guard tap blocks the most imminent active attack in that column; guarding
            //    a column with no incoming attack (while others ARE coming) is a mistake.
            if (tap && btn >= 0 && activeCount() > 0) {
                int best = -1; float bestp = -1.0f;
                for (int i = 0; i < MAX_ATK; i++)
                    if (atk_[i].active && atk_[i].dir == btn && atk_[i].p > bestp) { best = i; bestp = atk_[i].p; }
                if (best >= 0) { atk_[best].active = false; score_++; blockDir_ = btn; flashT_ = BLOCK_FLASH; }
                else takeHit();                        // wrong column = a hit
            }
            // 2) advance attacks; any that reach the pet unblocked land a hit
            for (int i = 0; i < MAX_ATK; i++) {
                if (!atk_[i].active) continue;
                atk_[i].p += dt / atk_[i].travel;
                if (atk_[i].p >= 1.0f) { atk_[i].active = false; takeHit(); }
            }
            // 3) spawn on a cadence, up to the current concurrency cap (timer frozen at the cap)
            if (activeCount() < maxConcurrent()) {
                spawnT_ -= dt;
                if (spawnT_ <= 0.0f) { spawnAttack(); spawnT_ = curGap(); }
            }
            if (lives_ <= 0) { award(); phase_ = OVER; }
            break;
        }

        case OVER:
            if (tap) app().setScene(SceneId::Activities, Slide::Iris);
            break;
    }
}

// small helpers for drawing
static void draw_heart(int cx, int cy, uint16_t c)
{
    fb.fillCircle(cx - 3, cy - 1, 3, c);
    fb.fillCircle(cx + 3, cy - 1, 3, c);
    fb.fillTriangle(cx - 6, cy + 1, cx + 6, cy + 1, cx, cy + 7, c);
}

void SceneBulwark::render()
{
    fb.fillScreen(rgb565(26, 30, 42));
    gfx_text(16, 10, 3, col::accent, "BULWARK");

    // hearts (remaining guard)
    for (int i = 0; i < LIVES_START; i++)
        draw_heart(GAME_W - 20 - i * 18, 20, i < lives_ ? rgb565(240, 80, 90) : rgb565(70, 60, 66));

    if (phase_ == READY) {
        gfx_text(20, 60,  1, col::white, "Attacks fly in from 3 sides.");
        gfx_text(20, 76,  1, col::white, "Tap that SIDE of the screen");
        gfx_text(20, 92,  1, col::white, "(left / middle / right) to guard");
        gfx_text(20, 108, 1, col::white, "before each one lands!");
        gfx_text(20, 128, 1, col::dim,   "Trains Endurance (END) + reflexes.");
        mg_energy_readout(20, 140, (int)app().pet.energy());
        gfx_text(48, 206, 2, col::good, "TAP TO START");
        return;
    }

    gfx_text(20, 40, 1, col::accent, "Blocks %d", score_);

    // the defender (big sprites capped so they don't dwarf the scene)
    LGFX_Sprite* spr = app().creatures.sprite(app().pet.creatureIndex());
    if (spr) gfx_blit_sprite_fit_bottom(spr, PET_CX, PET_CY + 30, 96, 96, SPRITE_TRANSP);
    else     gfx_blit(SPR_FALLBACK, PET_CX, PET_CY - SPRITE_H / 2);

    // successful-block shield flash on the guarded side (fades out over its lifetime)
    if (flashT_ > 0.0f) {
        uint16_t sc = mix565(rgb565(26, 30, 42), rgb565(120, 220, 255), flashT_ / BLOCK_FLASH);
        if      (blockDir_ == 0) fb.fillRoundRect(PET_CX - 44, PET_CY - 22, 8, 44, 3, sc);
        else if (blockDir_ == 2) fb.fillRoundRect(PET_CX + 36, PET_CY - 22, 8, 44, 3, sc);
        else                     fb.fillRoundRect(PET_CX - 22, PET_CY - 46, 44, 8, 3, sc);
    }

    // incoming attack projectiles (may be several at once)
    for (int i = 0; i < MAX_ATK; i++) {
        if (!atk_[i].active) continue;
        float p = atk_[i].p;
        int ax = PET_CX, ay = PET_CY;
        if      (atk_[i].dir == 0) ax = (int)(-12 + ((PET_CX - 34) - (-12)) * p);
        else if (atk_[i].dir == 2) ax = (int)((GAME_W + 12) + ((PET_CX + 34) - (GAME_W + 12)) * p);
        else                       ay = (int)(-12 + ((PET_CY - 40) - (-12)) * p);
        int r = 8 + (int)(6.0f * p);
        uint16_t ac = (p > 0.75f) ? rgb565(255, 90, 70) : rgb565(220, 120, 60);
        fb.fillCircle(ax, ay, r, ac);
        fb.drawCircle(ax, ay, r, rgb565(255, 220, 180));
    }

    // guard buttons fill their columns (arrows: left / up / right); the whole column above
    // each is a live tap-zone, but the buttons alone signal that (no distracting dividers).
    for (int i = 0; i < 3; i++) {
        int x = i * COL_W + GUARD_M, w = COL_W - 2 * GUARD_M;
        fb.fillRoundRect(x, GUARD_Y, w, GUARD_H, 8, rgb565(58, 64, 88));
        fb.drawRoundRect(x, GUARD_Y, w, GUARD_H, 8, col::accent);
        int cx = x + w / 2, cy = GUARD_Y + GUARD_H / 2;
        uint16_t ar = col::white;
        if      (i == 0) fb.fillTriangle(cx - 13, cy, cx + 9, cy - 13, cx + 9, cy + 13, ar);  // left
        else if (i == 2) fb.fillTriangle(cx + 13, cy, cx - 9, cy - 13, cx - 9, cy + 13, ar);  // right
        else             fb.fillTriangle(cx, cy - 13, cx - 13, cy + 9, cx + 13, cy + 9, ar);  // up
    }

    // took-a-hit red frame
    if (hurtT_ > 0.0f) {
        uint16_t hc = rgb565(230, 40, 40);
        fb.fillRect(0, 0, GAME_W, 5, hc); fb.fillRect(0, GAME_H - 5, GAME_W, 5, hc);
        fb.fillRect(0, 0, 5, GAME_H, hc); fb.fillRect(GAME_W - 5, 0, 5, GAME_H, hc);
    }

    if (phase_ == OVER) {
        mg_over_card("GUARD DOWN");
        mg_center(MG_CARD_Y + 40, 2, col::white, "Blocks %d", score_);
        mg_center(MG_CARD_Y + 68, 1, tired_ ? col::warn : col::good,
                  "+%d END  +%d AGI%s", gainEnd_, gainAgi_, tired_ ? "  tired!" : "");
    }
}

void SceneBulwark::onInput(const Input& in)
{
    if (!in.pressed) return;
    tapped_ = true;
    pendingBtn_ = btnAt(in.x, in.y);
}
