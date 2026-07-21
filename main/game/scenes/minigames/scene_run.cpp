#include "scene_run.hpp"
#include "engine/gfx.hpp"
#include "core/app.hpp"
#include "sim/creatures.hpp"        // Creature (runner sprite)
#include "assets/sprites.hpp"   // spr_unknown_data (fallback), SPRITE_*
#include "assets/tiles.hpp"     // grass_tile / dirt_tile (scrolling ground)
#include "esp_random.h"
#include "esp_log.h"
#include <cmath>

static const Sprite SPR_FALLBACK { spr_unknown_data, SPRITE_W, SPRITE_H, SPRITE_TRANSP };  // "?" when a sprite can't be shown

static const int   GROUND_Y   = 250;   // ground surface (pet's feet rest here)
static const int   PET_X      = 58;    // pet's fixed horizontal position
// Hit box is a FIXED "core" size regardless of how big the sprite is drawn, so play
// stays fair (and the run-under-flyer timing stays tuned) whatever the creature's size.
static const int   PET_HALF_W = 15;    // half hit-box width
static const int   PET_BODY_H = 38;    // hit-box height (feet up; a bit forgiving)

// Obstacle types. yTop/yBot are px ABOVE the ground (bottom 0 = sits on the ground).
// A flyer floats above head height: jumping into it collides, so you must NOT jump.
enum { OB_LOW, OB_TALL, OB_WIDE, OB_FLYER, OB_COUNT };
struct ObDef { int w, yTop, yBot; };
static const ObDef OB[OB_COUNT] = {
    { 14, 30,  0 },   // LOW   - jump
    { 14, 46,  0 },   // TALL  - jump higher, tighter timing
    { 34, 26,  0 },   // WIDE  - jump earlier, span it
    { 22, 78, 50 },   // FLYER - run UNDER (do not jump)
};
// score at which each new type starts appearing
static const int TALL_AT  = 4;
static const int WIDE_AT  = 9;
static const int FLYER_AT = 15;

static const float SPEED0     = 95.0f;   // starting scroll speed (px/s)
static const float SPEED_MAX  = 240.0f;  // base top speed (before score bonuses)
static const float SPEED_STEP = 20.0f;   // +top speed for every 10 points of score
static const float SPEED_RAMP = 6.0f;    // +px/s for each second of running
static const float GRAVITY    = 1000.0f; // px/s^2
static const float JUMP_V0    = 380.0f;  // jump impulse (apex ~72px, airtime ~0.76s)
static const float JUMP_APEX  = 72.0f;   // for the shadow scaling

// Ground hurdles are spaced by TIME, not distance, so they stay clearable at any speed
// (the on-screen gap grows as you accelerate). Hurdle->hurdle gaps can be TIGHT and get
// tighter as the score climbs.
static const float JUMP_GAP_MIN0  = 0.95f;   // hurdle->hurdle min gap at score 0
static const float JUMP_GAP_FLOOR = 0.66f;   // ...tightens down to this (still clearable)
static const float JUMP_GAP_MAX   = 1.60f;   // hurdle->hurdle max gap
static const float JUMP_TIGHTEN   = 0.012f;  // min gap reduction per point of score
static const float FIRST_DELAY    = 1.40f;   // seconds before the first obstacle

// Flyers are a SEPARATE overlay stream (don't score). They spawn only into a safe pocket
// of the ground schedule: at least `safe` seconds clear of the last AND next hurdle, so a
// flyer is never directly over a hurdle (impossible jump) but can sit close for tight play.
static const float FLY_PERIOD_MIN = 1.6f;    // min seconds between flyer attempts
static const float FLY_PERIOD_MAX = 3.4f;    // max seconds between flyer attempts
static const float FLY_SAFE0      = 0.60f;   // min clearance from a hurdle at score 0
static const float FLY_SAFE_FLOOR = 0.42f;   // ...tightens down to this (still possible)
static const float FLY_TIGHTEN    = 0.006f;  // clearance reduction per point of score

// Reward from a finished run (flat: gain = score * per-point / STAT_DIV). Running trains
// Agility mainly and Max HP (stamina) a bit; sharing the game builds friendship.
static const int   AGI_PER_PT     = 3;
static const int   HP_PER_PT      = 6;
static const int   STAT_DIV       = 5;   // damp overall stat gain per run (bigger = slower)

// Training is energy-gated (see docs/training-and-energy.md): a run costs stamina, and if
// the pet is too tired the stat gains are scaled down proportionally. Bond isn't gated.
static const float ENERGY_RUN_BASE   = 12.0f;   // energy a run costs...
static const float ENERGY_RUN_PER_PT = 1.2f;    // ...plus this per point of score

static float rnd01() { return (float)(esp_random() & 0xFFFF) / 65535.0f; }

void SceneRun::reset()
{
    phase_ = READY;
    t_ = runTime_ = dist_ = 0.0f;
    speed_ = SPEED0;
    py_ = vy_ = 0.0f;
    grounded_ = true;
    spawnTimer_ = 0.0f;
    nextSpawnT_ = FIRST_DELAY;
    flyTimer_ = 0.0f;
    flyNextT_ = FLY_PERIOD_MIN + rnd01() * (FLY_PERIOD_MAX - FLY_PERIOD_MIN);
    score_ = 0;
    tapped_ = false;
    prevType_ = OB_LOW;
    pendingType_ = OB_LOW;      // first obstacle is always a plain hurdle (warm-up)
    rewarded_ = false;
    tired_ = false;
    gainAgi_ = gainHp_ = 0;
    for (int i = 0; i < MAX_H; i++) { hActive_[i] = false; hCounted_[i] = false; }
}

void SceneRun::award()
{
    if (rewarded_) return;
    rewarded_ = true;
    Pet& pet = app().pet;
    int rawAgi = score_ * AGI_PER_PT / STAT_DIV;
    int rawHp  = score_ * HP_PER_PT / STAT_DIV;

    // energy gate: not enough stamina -> proportionally reduced gains (and drain what's left)
    float cost  = ENERGY_RUN_BASE + (float)score_ * ENERGY_RUN_PER_PT;
    float have  = pet.energy();
    float ratio = (cost <= 0.0f) ? 1.0f : (have >= cost ? 1.0f : have / cost);
    tired_ = ratio < 0.999f;
    gainAgi_ = (int)(rawAgi * ratio);
    gainHp_  = (int)(rawHp  * ratio);

    pet.spendEnergy(have < cost ? have : cost);   // spend the run's cost, or drain what's left if short
    pet.trainStat(STAT_AGI,   (uint32_t)gainAgi_);
    pet.trainStat(STAT_MAXHP, (uint32_t)gainHp_);
    pet.addFriendship(2 + score_ / 8);   // sharing the game builds the bond (not energy-gated)
    pet.markSaved();                     // persist the run's gains
    ESP_LOGI("RUN", "reward: score=%d +%d AGI +%d HP (energy %.0f cost %.0f x%.2f)",
             score_, gainAgi_, gainHp_, have, cost, ratio);
}

void SceneRun::onEnter() { reset(); }

int SceneRun::pickType() const
{
    // pick from the GROUND obstacle types unlocked at the current score (flyers are a
    // separate overlay stream, not part of this sequence).
    int allowed[OB_COUNT]; int n = 0;
    allowed[n++] = OB_LOW;
    if (score_ >= TALL_AT) allowed[n++] = OB_TALL;
    if (score_ >= WIDE_AT) allowed[n++] = OB_WIDE;
    return allowed[esp_random() % (uint32_t)n];
}

float SceneRun::gapFor(int prev, int next) const
{
    // hurdle->hurdle gap: tight, tightening as the score climbs. (prev/next reserved for
    // future per-type tuning; flyers are handled by their own stream.)
    (void)prev; (void)next;
    float minG = JUMP_GAP_MIN0 - (float)score_ * JUMP_TIGHTEN;
    if (minG < JUMP_GAP_FLOOR) minG = JUMP_GAP_FLOOR;
    return minG + rnd01() * (JUMP_GAP_MAX - minG);
}

void SceneRun::spawnHurdle(int type)
{
    for (int i = 0; i < MAX_H; i++) {
        if (!hActive_[i]) {
            hActive_[i]  = true;
            hCounted_[i] = false;
            hx_[i]       = (float)GAME_W + 6.0f;
            hType_[i]    = (uint8_t)type;
            return;
        }
    }
    ESP_LOGW("RUN", "obstacle pool full (%d) - spawn dropped", MAX_H);   // shouldn't happen
}

void SceneRun::update(float dt)
{
    t_ += dt;
    bool tap = tapped_; tapped_ = false;

    if (phase_ == READY) {
        if (tap) phase_ = RUNNING;              // first tap starts (doesn't jump)
        return;
    }
    if (phase_ == OVER) {
        if (tap) app().setScene(SceneId::Menu, Slide::Iris); // tap to leave (iris)
        return;
    }

    // --- RUNNING ---
    runTime_ += dt;
    float speedCap = SPEED_MAX + (float)(score_ / 10) * SPEED_STEP;  // +20 per 10 points
    speed_ = SPEED0 + runTime_ * SPEED_RAMP;
    if (speed_ > speedCap) speed_ = speedCap;

    if (tap && grounded_) { vy_ = -JUMP_V0; grounded_ = false; }   // jump

    if (!grounded_) {                             // vertical physics
        vy_ += GRAVITY * dt;
        py_ += vy_ * dt;
        if (py_ >= 0.0f) { py_ = 0.0f; vy_ = 0.0f; grounded_ = true; }
    }

    // scroll the world; spawn obstacles on a time cadence (stays clearable at any speed).
    // The next type is chosen ahead of time so its gap can depend on the type pair.
    float d = speed_ * dt;
    dist_ += d;
    spawnTimer_ += dt;
    if (spawnTimer_ >= nextSpawnT_) {
        spawnTimer_ = 0.0f;
        spawnHurdle(pendingType_);
        prevType_    = pendingType_;
        pendingType_ = (uint8_t)pickType();               // queue the following one
        nextSpawnT_  = gapFor(prevType_, pendingType_);   // gap tuned to the pair
    }

    // Flyer overlay: independent hazard that overlaps the ground obstacles but only spawns
    // into a SAFE POCKET (>= `safe` seconds clear of the last AND next hurdle), so it's never
    // directly over a hurdle. `safe` shrinks with score -> flyers hug hurdles more tightly.
    if (score_ >= FLYER_AT) {
        flyTimer_ += dt;
        if (flyTimer_ >= flyNextT_) {
            float safe = FLY_SAFE0 - (float)score_ * FLY_TIGHTEN;
            if (safe < FLY_SAFE_FLOOR) safe = FLY_SAFE_FLOOR;
            if (spawnTimer_ >= safe && (nextSpawnT_ - spawnTimer_) >= safe) {
                spawnHurdle(OB_FLYER);
                flyTimer_ = 0.0f;
                flyNextT_ = FLY_PERIOD_MIN + rnd01() * (FLY_PERIOD_MAX - FLY_PERIOD_MIN);
            }
            // else: no safe pocket right now -> keep the timer elapsed and retry next frame
        }
    }

    // move / score / collide (AABB: pet box vs obstacle box), using the FIXED core box.
    const int   petL   = PET_X - PET_HALF_W, petR = PET_X + PET_HALF_W;
    const float petTop = (float)GROUND_Y + py_ - PET_BODY_H;
    const float petBot = (float)GROUND_Y + py_;    // pet's lowest point (feet)
    for (int i = 0; i < MAX_H; i++) {
        if (!hActive_[i]) continue;
        const ObDef& o = OB[hType_[i]];
        hx_[i] -= d;
        int hl = (int)hx_[i], hr = (int)hx_[i] + o.w;

        if (!hCounted_[i] && hr < petL) {                                   // cleared
            hCounted_[i] = true;
            if (hType_[i] != OB_FLYER) score_++;                            // flyers don't score
        }
        if (hr < -6) { hActive_[i] = false; continue; }                     // recycle

        float obTop = (float)(GROUND_Y - o.yTop);   // higher on screen (smaller y)
        float obBot = (float)(GROUND_Y - o.yBot);
        bool xover = (hl < petR) && (hr > petL);
        bool yover = (petTop < obBot) && (petBot > obTop);
        if (xover && yover) phase_ = OVER;
    }

    if (phase_ == OVER) award();   // just collided this frame -> grant stat gains once
}

void SceneRun::render()
{
    // sky + ground band
    fb.fillRect(0, 0, GAME_W, GROUND_Y, col::sky);
    // scrolling tiled ground: grass surface row + dirt below, shifted by distance run
    int scroll = (int)dist_;
    gfx_tile_region(0, GROUND_Y, GAME_W, TILE_H, grass_tile, TILE_W, TILE_H, scroll);
    gfx_tile_region(0, GROUND_Y + TILE_H, GAME_W, GAME_H - GROUND_Y - TILE_H, dirt_tile, TILE_W, TILE_H, scroll);

    // obstacles
    for (int i = 0; i < MAX_H; i++) {
        if (!hActive_[i]) continue;
        const ObDef& o = OB[hType_[i]];
        int hx  = (int)hx_[i];
        int top = GROUND_Y - o.yTop;
        int h   = o.yTop - o.yBot;
        if (hType_[i] == OB_FLYER) {
            // floating hazard (do NOT jump): purple body + little wings + eye
            uint16_t body = rgb565(160, 95, 225), wing = rgb565(205, 155, 245);
            fb.fillRoundRect(hx, top, o.w, h, 5, body);
            fb.fillTriangle(hx - 7, top + 3, hx, top + 1, hx, top + 13, wing);
            fb.fillTriangle(hx + o.w + 7, top + 3, hx + o.w, top + 1, hx + o.w, top + 13, wing);
            fb.fillCircle(hx + o.w - 5, top + 8, 2, col::black);
        } else {
            fb.fillRect(hx, top, o.w, h, rgb565(70, 48, 34));
            fb.fillRect(hx, top, o.w, 5, rgb565(230, 90, 70));   // red cap = jump me
            fb.drawRect(hx, top, o.w, h, col::black);
        }
    }

    // pet sprite (drives shadow width + the draw); hitbox stays fixed, this is cosmetic
    LGFX_Sprite* spr = app().creatures.sprite(app().pet.creatureIndex());  // lazy-loaded + cached
    int psw = spr ? spr->width() : SPRITE_W;

    // shadow (scales with the pet's width, shrinks as it rises)
    float h01 = -py_ / JUMP_APEX; if (h01 > 1.0f) h01 = 1.0f;
    int shBase = psw * 5 / 12;
    int shw = shBase - (int)(shBase * 0.55f * h01);
    fb.fillEllipse(PET_X, GROUND_Y + 4, shw, 4, rgb565(60, 48, 32));

    // pet: feet on the ground (+ jump offset), gentle run-bob
    int bob = grounded_ ? (int)(2.5f * sinf(t_ * 18.0f)) : 0;
    int feet = GROUND_Y + (int)py_ + bob;
    if (spr) gfx_blit_sprite_bottom(spr, PET_X, feet, SPRITE_TRANSP);
    else     gfx_blit(SPR_FALLBACK, PET_X, feet - SPRITE_H / 2);

    // HUD
    gfx_text(8, 8, 3, col::white, "%d", score_);
    gfx_text(GAME_W - 74, 10, 1, col::dim, "spd %d", (int)speed_);

    if (phase_ == READY) {
        gfx_text(84, 92, 3, col::accent, "RUN!");
        gfx_text(24, 136, 1, col::white, "Tap to jump red hurdles.");
        gfx_text(24, 152, 1, col::white, "Run UNDER purple flyers.");
        gfx_text(24, 168, 1, col::white, "It speeds up as you go.");
        int en = (int)app().pet.energy();
        gfx_text(24, 186, 1, en < 25 ? col::warn : col::dim, "Energy %d/100%s", en, en < 25 ? "  (tired!)" : "");
        gfx_text(48, 210, 2, col::good, "TAP TO START");
    } else if (phase_ == OVER) {
        fb.fillRoundRect(28, 104, GAME_W - 56, 96, 8, col::panel);
        fb.drawRoundRect(28, 104, GAME_W - 56, 96, 8, col::warn);
        gfx_text(62, 116, 2, col::warn, "GAME OVER");
        gfx_text(76, 142, 2, col::white, "Score %d", score_);
        gfx_text(52, 168, 1, tired_ ? col::warn : col::good,
                 "+%d AGI  +%d HP%s", gainAgi_, gainHp_, tired_ ? "  tired!" : "");
        gfx_text(74, 184, 1, col::dim, "Tap to exit");
    }
}

void SceneRun::onInput(const Input& in)
{
    if (in.pressed) tapped_ = true;
}
