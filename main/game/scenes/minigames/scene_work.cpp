#include "scene_work.hpp"
#include "core/app.hpp"
#include "engine/gfx.hpp"
#include "engine/audio/sfx.hpp"
#include "minigame.hpp"
#include "sim/economy.hpp"
#include "esp_random.h"
#include <cstring>
#include <cstdio>

// layout
static const int PAD_X   = 12;
static const int BELT_Y  = 116;                  // parcels ride along this line
static const int BELT_H  = 44;
static const int CHUTE_X = 34;                   // a parcel is lost once it passes here
static const int BIN_Y   = 214;
static const int BIN_H   = 62;
static const int BIN_W   = (GAME_W - 2 * PAD_X - 2 * 8) / 3;

// Difficulty ramp. The belt gets faster and the queue thicker every PER_LEVEL sorts; both
// flatten out so a very long shift stays humanly possible rather than becoming a coin flip.
//
// The pace of DECISIONS is the gap, not the speed -- at a 2.1s gap the opening was a parcel
// every two seconds, which played as waiting. The bases moved up and the steps came down by
// the same proportion, so the shift now opens at roughly what used to be level 3 and still
// converges on the same ceiling at the same level. Only the boring part was removed.
static const float SPEED_BASE = 42.0f, SPEED_STEP = 4.0f,  SPEED_MAX = 96.0f;
static const float GAP_BASE   = 1.45f, GAP_STEP   = 0.06f, GAP_MIN   = 0.62f;

static uint16_t shape_color(int s)
{
    switch (s) {
        case 0:  return rgb565(226, 118,  92);
        case 1:  return rgb565( 96, 176, 226);
        default: return rgb565(214, 190,  84);
    }
}

// Shapes are drawn rather than sprited: three primitives read cleanly at 20px and cost no art.
static void draw_shape(int s, int cx, int cy, int r, uint16_t c)
{
    switch (s) {
        case 0: fb.fillCircle(cx, cy, r, c); fb.drawCircle(cx, cy, r, col::black); break;
        case 1: fb.fillRoundRect(cx - r, cy - r, r * 2, r * 2, 3, c);
                fb.drawRoundRect(cx - r, cy - r, r * 2, r * 2, 3, col::black); break;
        default: fb.fillTriangle(cx, cy - r, cx - r, cy + r, cx + r, cy + r, c);
                 fb.drawTriangle(cx, cy - r, cx - r, cy + r, cx + r, cy + r, col::black); break;
    }
}

static Rect bin_rect(int i) { return Rect{ PAD_X + i * (BIN_W + 8), BIN_Y, BIN_W, BIN_H }; }

float SceneWork::beltSpeed() const
{
    float v = SPEED_BASE + SPEED_STEP * (float)level_;
    return v > SPEED_MAX ? SPEED_MAX : v;
}

float SceneWork::spawnGap() const
{
    float g = GAP_BASE - GAP_STEP * (float)level_;
    return g < GAP_MIN ? GAP_MIN : g;
}

void SceneWork::shuffleBins()
{
    // Fisher-Yates over three elements. Done at the start of every shift AND at each
    // difficulty step, so the position you just learned stops being worth anything -- which
    // is the whole difference between this and a colour-matching reflex test.
    for (int i = 0; i < SHAPE_N; i++) binShape_[i] = (uint8_t)i;
    for (int i = SHAPE_N - 1; i > 0; i--) {
        int j = (int)(esp_random() % (uint32_t)(i + 1));
        uint8_t t = binShape_[i]; binShape_[i] = binShape_[j]; binShape_[j] = t;
    }
}

void SceneWork::onEnter()
{
    phase_ = READY;
    for (auto& p : belt_) p.used = false;
    head_ = tail_ = 0;
    spawnT_ = 0.6f;                 // a beat before the first parcel, so the bins can be read
    score_ = missed_ = level_ = 0;
    bits_ = 0;
    doneT_ = flashT_ = swapT_ = 0.0f;
    paid_ = false;
    shuffleBins();
}

void SceneWork::spawn()
{
    if (belt_[tail_].used) return;                    // belt full; skip this beat
    belt_[tail_].shape = (uint8_t)(esp_random() % SHAPE_N);
    belt_[tail_].x     = GAME_W + 20.0f;
    belt_[tail_].used  = true;
    tail_ = (tail_ + 1) % BELT_MAX;
}

void SceneWork::miss()
{
    missed_++;
    flashT_ = 0.35f;
    flashGood_ = false;
    if (missed_ >= LIVES) finish();
}

void SceneWork::sortInto(int bin)
{
    if (!belt_[head_].used) { sfx::play(sfx::kDenied); return; }   // nothing at the chute yet

    const bool right = (binShape_[bin] == belt_[head_].shape);
    belt_[head_].used = false;
    head_ = (head_ + 1) % BELT_MAX;

    if (!right) { sfx::play(sfx::kDenied); miss(); return; }

    sfx::play(sfx::kSelect);
    score_++;
    flashT_ = 0.35f;
    flashGood_ = true;

    // Step up, and swap the bins as we do. The swap is announced, because a difficulty
    // increase you cannot see coming reads as the game cheating.
    if (score_ % PER_LEVEL == 0) {
        level_++;
        shuffleBins();
        swapT_ = 1.1f;
        sfx::play(sfx::kLevelUp);
    }
}

void SceneWork::finish()
{
    if (paid_) return;
    phase_ = OVER;
    doneT_ = 0.0f;

    // Skill pays, but into a ceiling. A shift is the floor under priced treatment, so a
    // superb run should be an hour's pocket money -- not better per minute than a battle.
    bits_ = (uint32_t)(2 + score_ / 2);
    if (bits_ > (uint32_t)BITS_CAP) bits_ = (uint32_t)BITS_CAP;

    app().economy.earn(bits_);
    app().economy.flush();
    paid_ = true;
}

void SceneWork::update(float dt)
{
    if (flashT_ > 0) flashT_ -= dt;
    if (swapT_  > 0) swapT_  -= dt;

    if (phase_ == OVER)  { doneT_ += dt; return; }
    if (phase_ != WORKING) return;

    spawnT_ -= dt;
    if (spawnT_ <= 0.0f) { spawn(); spawnT_ = spawnGap(); }

    const float v = beltSpeed();
    for (auto& p : belt_) if (p.used) p.x -= v * dt;

    // Only the head can fall off: the belt is a queue, so nothing overtakes.
    if (belt_[head_].used && belt_[head_].x < (float)(CHUTE_X - 24)) {
        belt_[head_].used = false;
        head_ = (head_ + 1) % BELT_MAX;
        miss();
    }
}

void SceneWork::render()
{
    fb.fillScreen(col::panel);
    gfx_text(PAD_X, 14, 2, col::accent, "SORTING SHIFT");

    if (phase_ == READY) {
        gfx_text_wrap(PAD_X, 48, GAME_W - 2 * PAD_X, 1, col::white,
                      "Go to work. This allows you to gain Bits even when your pet is sick "
                      "or injured.", 4);
        gfx_text_wrap(PAD_X, 104, GAME_W - 2 * PAD_X, 1, col::dim,
                      "Parcels come down the belt. Tap the bin that takes the shape at the "
                      "front.\n\n"
                      "The bins swap as the pace picks up, so read them -- don't memorise "
                      "them. Three dropped parcels ends the shift.", 4);
        gfx_text(PAD_X + 26, 244, 2, col::good, "TAP TO START");
        draw_back();
        return;
    }

    // --- HUD ---
    gfx_text(PAD_X, 44, 1, col::dim, "Sorted");
    gfx_text(PAD_X + 44, 42, 2, col::white, "%d", score_);
    char lv[16];
    snprintf(lv, sizeof lv, "Lv %d", level_ + 1);
    gfx_text(GAME_W - PAD_X - (int)strlen(lv) * 6, 44, 1, col::accent, "%s", lv);

    // Lives as pips: three is few enough to read at a glance without a number.
    for (int i = 0; i < LIVES; i++) {
        const int cx = GAME_W / 2 - 18 + i * 18;
        if (i < LIVES - missed_) fb.fillCircle(cx, 48, 5, col::good);
        else                     fb.drawCircle(cx, 48, 5, col::dim);
    }

    // --- belt ---
    fb.fillRoundRect(PAD_X, BELT_Y - BELT_H / 2, GAME_W - 2 * PAD_X, BELT_H, 6, col::card);
    fb.drawRoundRect(PAD_X, BELT_Y - BELT_H / 2, GAME_W - 2 * PAD_X, BELT_H, 6, col::dim);
    fb.drawFastVLine(CHUTE_X + 22, BELT_Y - BELT_H / 2, BELT_H,
                     flashT_ > 0 ? (flashGood_ ? col::good : col::warn) : col::dim);

    fb.setClipRect(PAD_X + 1, BELT_Y - BELT_H / 2 + 1, GAME_W - 2 * PAD_X - 2, BELT_H - 2);
    for (int k = 0; k < BELT_MAX; k++) {
        const Parcel& p = belt_[(head_ + k) % BELT_MAX];
        if (p.used) draw_shape(p.shape, (int)p.x, BELT_Y, 15, shape_color(p.shape));
    }
    fb.clearClipRect();

    if (swapT_ > 0 && phase_ == WORKING) {
        const char* s = "BINS SWAPPED!";
        gfx_text((GAME_W - (int)strlen(s) * 12) / 2, BELT_Y + 34, 2, col::warn, "%s", s);
    }

    // --- bins ---
    for (int i = 0; i < SHAPE_N; i++) {
        Rect b = bin_rect(i);
        b.fill(col::card, 8);
        b.outline(swapT_ > 0 ? col::warn : col::dim, 8);
        draw_shape(binShape_[i], b.x + b.w / 2, b.y + 28, 16, shape_color(binShape_[i]));
    }

    if (phase_ == OVER) {
        mg_over_card("SHIFT OVER", bits_);
        mg_center(MG_CARD_Y + 40, 2, col::white, "Sorted %d", score_);
        mg_center(MG_CARD_Y + 68, 1, col::dim, "reached Lv %d", level_ + 1);
    }
}

void SceneWork::onInput(const Input& in)
{
    if (!in.pressed) return;

    if (phase_ == READY) {
        if (kBack.contains(in)) { app().setScene(SceneId::Menu, Slide::Back); return; }
        phase_ = WORKING;
        return;
    }
    if (phase_ == OVER) {
        if (doneT_ > 0.4f) app().setScene(SceneId::Menu, Slide::Back);
        return;
    }

    for (int i = 0; i < SHAPE_N; i++)
        if (bin_rect(i).contains(in)) { sortInto(i); return; }
}
