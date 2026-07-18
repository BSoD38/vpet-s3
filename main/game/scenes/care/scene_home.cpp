#include "scene_home.hpp"
#include "core/app.hpp"
#include "engine/gfx.hpp"
#include "sim/creatures.hpp"        // Creature (current sprite)
#include "engine/clock.hpp"            // clock_datetime (torn-read-safe RTC snapshot)
#include "engine/drivers.hpp"          // BAT_analogVolts
#include "assets/sprites.hpp"   // spr_unknown_data (fallback sprite)
#include "assets/tiles.hpp"     // grass_tile / dirt_tile (ground)
#include "esp_random.h"
#include <cmath>
#include <cstring>

static const Sprite SPR_FALLBACK { spr_unknown_data, SPRITE_W, SPRITE_H, SPRITE_TRANSP };  // "?" when a sprite can't be shown

static const int HORIZON = GAME_H * 2 / 3;
static const int PET_CX  = GAME_W / 2;
static const int PET_FEET = HORIZON - 3;    // baseline the creature's feet rest on (fits sprites up to 144 tall)
static const int PET_HIT_MARGIN = 8;        // petting-zone slack around the sprite's box

// bottom action bar (quick care actions) + top-right menu button
static const int   ACT_N  = 4;
static const int   ACT_W  = 52, ACT_H = 52, ACT_GAP = 5, ACT_X0 = 8, ACT_Y = 260;
static const int   MB_X   = 204, MB_Y = 4, MB_W = 32, MB_H = 24;   // visible button
// Touch target = the top-right corner strip ABOVE the stat bars (bars start at
// y29). Kept generous in x but capped at y28 so it never overlaps the HAP bar.
static const int   MB_HX = 192, MB_HY = 0, MB_HW = GAME_W - 192, MB_HH = 28;
static const char* ACT_LABEL[4] = { "Feed", "Clean", "Heal", "Sleep" };
static int act_x(int i) { return ACT_X0 + i * (ACT_W + ACT_GAP); }

// Petting: accumulate finger travel; every PET_CHUNK_DIST px of rubbing earns a chunk.
// (Distance-based, not time-based, so it's robust to the touch controller repeating
// coordinates across polls.)
static const float PET_CHUNK_DIST  = 200.0f;  // px of rubbing per happiness chunk
static const float PET_MOVE_DEADZ  = 1.0f;    // ignore <=1px jitter
static const float PET_CHUNK       = 35.0f;   // happiness granted per chunk
// Poke: a quick tap (short + little movement), detected on release. Poking is
// cumulative — it takes a random 3..6 pokes (each with a short gap) to earn a
// happiness reward; every poke bounces the pet, and the reward adds sparkles.
static const int   POKE_CHUNK      = 30;      // happiness once a poke run completes
static const float POKE_MAX_TIME   = 0.35f;   // a tap is shorter than this
static const float POKE_MAX_DIST   = 14.0f;   // ...and moves less than this (total px)
static const float POKE_HOP_TIME   = 0.25f;   // quick poke hop; also the min gap between pokes
static const float PLAY_COOLDOWN   = 1.5f;    // lockout after a pet chunk or a completed poke run;
                                              // the hearts / sparkles linger for exactly this long
static const float REFUSE_TIME     = 0.6f;    // "no" head-shake duration
// Bonding: petting and completed poke runs nudge the friendship meter (persisted at
// the next autosave/care action, matching how play() defers happiness saves).
static const int   FR_PET          = 2;       // per earned rub chunk
static const int   FR_POKE         = 5;       // per completed poke run

static uint16_t bar_color(float v01)
{
    return v01 < 0.30f ? col::warn : col::good;
}

// A small stacked "poop" with a dark outline so it reads against the brown ground.
static void draw_poop(int x, int y)
{
    const uint16_t body = rgb565(74, 46, 20);
    const uint16_t edge = rgb565(30, 18, 8);
    fb.fillCircle(x, y,      8, body);
    fb.fillCircle(x, y - 7,  6, body);
    fb.fillCircle(x, y - 13, 4, body);
    fb.drawCircle(x, y,      8, edge);
    fb.drawCircle(x, y - 7,  6, edge);
}

static void draw_heart(int x, int y, int s, uint16_t c)
{
    fb.fillCircle(x - s / 2, y, s / 2, c);
    fb.fillCircle(x + s / 2, y, s / 2, c);
    fb.fillTriangle(x - s, y + s / 4, x + s, y + s / 4, x, y + s + s / 3, c);
}

// --- action-icon glyphs, drawn centered at (x,y) ---
static void icon_feed(int x, int y, bool dim)          // apple
{
    fb.fillRect(x - 1, y - 11, 2, 5, rgb565(120, 80, 40));
    fb.fillCircle(x + 4, y - 8, 3, rgb565(90, 190, 90));
    fb.fillCircle(x, y + 1, 9, dim ? rgb565(120, 85, 80) : rgb565(230, 70, 60));
}
static void icon_clean(int x, int y)                   // poop with a red slash
{
    uint16_t br = rgb565(74, 46, 20), rd = rgb565(235, 70, 60);
    fb.fillCircle(x, y + 4, 6, br);
    fb.fillCircle(x, y - 1, 4, br);
    fb.fillCircle(x, y - 5, 3, br);
    fb.drawLine(x - 11, y + 9, x + 11, y - 9, rd);
    fb.drawLine(x - 11, y + 8, x + 11, y - 10, rd);
    fb.drawLine(x - 10, y + 10, x + 12, y - 8, rd);
}
static void icon_heal(int x, int y)                    // medical cross
{
    uint16_t g = rgb565(90, 205, 110);
    fb.fillRect(x - 3, y - 10, 6, 20, g);
    fb.fillRect(x - 10, y - 3, 20, 6, g);
}
static void icon_lights(int x, int y, bool asleep, uint16_t bg)  // moon (asleep) / sun (awake)
{
    if (asleep) {
        uint16_t ye = rgb565(240, 225, 120);
        fb.fillCircle(x, y, 9, ye);
        fb.fillCircle(x + 4, y - 3, 8, bg);          // carve a crescent
    } else {
        uint16_t ye = rgb565(255, 210, 70);
        fb.fillCircle(x, y, 6, ye);
        for (int a = 0; a < 8; a++) {
            float th = a * 0.7854f;
            fb.drawLine(x + (int)(8 * cosf(th)),  y + (int)(8 * sinf(th)),
                        x + (int)(12 * cosf(th)), y + (int)(12 * sinf(th)), ye);
        }
    }
}

void SceneHome::update(float dt)
{
    Pet& pet = app().pet;
    const PetState& p = pet.state();

    t_ += dt;
    if (pokeTarget_ == 0) pokeTarget_ = 3 + (int)(esp_random() % 4);   // this run needs 3..6 pokes

    // Petting zone tracks the creature's actual sprite size (feet-anchored baseline).
    LGFX_Sprite* petSpr = app().creatures.sprite(pet.creatureIndex());
    int psw = petSpr ? petSpr->width()  : SPRITE_W;
    int psh = petSpr ? petSpr->height() : SPRITE_H;
    int bodyCy = PET_FEET - psh / 2;              // no bob here: keep the hit box stable
    int hzW = psw / 2 + PET_HIT_MARGIN, hzH = psh / 2 + PET_HIT_MARGIN;
    bool onPetZone = hit(tx_, ty_, PET_CX - hzW, bodyCy - hzH, hzW * 2, hzH * 2);
    bool interactive = p.stage != STAGE_EGG && !p.lightsOff && !p.sick;
    bool overPet = interactive && onPetZone;    // can actually pet/poke
    overPet_ = overPet;
    rubbingNow_ = false;
    bool justPressed  = down_ && !wasDown_;
    bool justReleased = !down_ && wasDown_;

    // "no" wiggle: refused from the menu (e.g. feed while sick), or from touching a sick pet
    if (pet.checkRefused()) refuseTimer_ = REFUSE_TIME;
    if (justPressed && onPetZone && p.sick) refuseTimer_ = REFUSE_TIME;

    if (justPressed) {
        // arm a gesture only if the touch begins on the creature
        touchActive_ = overPet;
        touchDur_ = 0; rubDist_ = 0; rubProgress_ = 0;
        prevx_ = tx_; prevy_ = ty_;
    }

    if (touchActive_ && down_) {
        touchDur_ += dt;
        int dx = tx_ - prevx_, dy = ty_ - prevy_;
        float move = sqrtf((float)(dx * dx + dy * dy));
        lastMove_ = move;                                    // debug
        float eff = (move > PET_MOVE_DEADZ) ? move : 0.0f;   // filter jitter
        rubDist_ += eff;                                     // total travel (for poke test)
        if (overPet && eff > 0.0f) {                         // accumulate rub distance
            rubbingNow_ = true;
            if (playCooldown_ <= 0.0f) {                     // not on cooldown
                rubProgress_ += eff;
                if (rubProgress_ >= PET_CHUNK_DIST) {        // earned a chunk
                    rubProgress_ = 0.0f;
                    pet.play(PET_CHUNK);
                    pet.addFriendship(FR_PET);
                    playCooldown_ = PLAY_COOLDOWN;            // hearts linger for the cooldown
                }
            }
        }
    }

    if (justReleased && touchActive_) {
        // a short, low-movement touch counts as a poke (not a rub); poking is
        // cumulative — a run of 3..6 pokes earns the happiness reward.
        if (pokeCd_ <= 0.0f && touchDur_ < POKE_MAX_TIME && rubDist_ < POKE_MAX_DIST) {
            hopTimer_ = POKE_HOP_TIME;          // every poke bounces the pet
            if (++pokeCount_ >= pokeTarget_) {  // run complete -> reward + full lockout
                pet.play((float)POKE_CHUNK);
                pet.addFriendship(FR_POKE);
                sparkTimer_ = PLAY_COOLDOWN;    // sparkles linger for the whole cooldown
                pokeCd_     = PLAY_COOLDOWN;    // can't poke again until they clear
                pokeCount_  = 0;
                pokeTarget_ = 3 + (int)(esp_random() % 4);
            } else {
                pokeCd_ = POKE_HOP_TIME;        // brief gap before the next poke counts
            }
        }
        touchActive_ = false;
    }

    prevx_ = tx_; prevy_ = ty_; wasDown_ = down_;
    if (hopTimer_ > 0)     hopTimer_ -= dt;
    if (sparkTimer_ > 0)   sparkTimer_ -= dt;
    if (pokeCd_ > 0)       pokeCd_ -= dt;
    if (playCooldown_ > 0) playCooldown_ -= dt;
    if (refuseTimer_ > 0)  refuseTimer_ -= dt;
}

void SceneHome::render()
{
    Pet& pet = app().pet;
    const PetState& p = pet.state();

    // background
    fb.fillRect(0, 0, GAME_W, HORIZON, col::sky);
    // ground: a row of grass tiles at the horizon, dirt tiles filling below
    gfx_tile_region(0, HORIZON, GAME_W, TILE_H, grass_tile, TILE_W, TILE_H);
    gfx_tile_region(0, HORIZON + TILE_H, GAME_W, GAME_H - HORIZON - TILE_H, dirt_tile, TILE_W, TILE_H);

    // poop blobs on the ground
    for (int i = 0; i < p.poop; i++)
        draw_poop(34 + i * 44, HORIZON + 34);

    // pet: egg vs hatched. Gentle idle bob, a quick hop when poked, and a
    // side-to-side "no" head-shake when refusing an action.
    int wig = refuseTimer_ > 0.0f
                  ? (int)(7.0f * sinf(t_ * 28.0f) * (refuseTimer_ / REFUSE_TIME)) : 0;
    int cx = PET_CX + wig;
    int hop = hopTimer_ > 0.0f ? -(int)(12.0f * (hopTimer_ / POKE_HOP_TIME)) : 0;
    int feet = PET_FEET + (int)(4.0f * sinf(t_ * 2.0f)) + hop;   // baseline + idle bob + poke hop
    LGFX_Sprite* spr = app().creatures.sprite(pet.creatureIndex());   // lazy-loaded + cached
    int psh = spr ? spr->height() : SPRITE_H;
    int cy = feet - psh / 2;                                    // body center (for ring/hearts/markers)
    if (spr) gfx_blit_sprite_bottom(spr, cx, feet, SPRITE_TRANSP);
    else     gfx_blit(SPR_FALLBACK, cx, feet - SPRITE_H / 2);   // 48px "?" fallback, feet-anchored

    // Rub-progress ring: only while actively able to pet (not during cooldown).
    // Pink fill = progress to the next happiness chunk; bright when rubbing, dim when still.
    if (touchActive_ && overPet_ && playCooldown_ <= 0.0f && rubDist_ > POKE_MAX_DIST) {
        fb.fillArc(cx, cy, 30, 35, 0, 360, rgb565(50, 52, 62));
        float p = rubProgress_ / PET_CHUNK_DIST;
        if (p > 1.0f) p = 1.0f;
        if (p > 0.0f) {
            uint16_t fg = rubbingNow_ ? rgb565(255, 120, 160) : col::dim;
            fb.fillArc(cx, cy, 30, 35, 0, (int)(p * 360.0f), fg);
        }
    }

    // Petting reward: hearts linger for the cooldown.
    if (playCooldown_ > 0.0f) {
        const uint16_t pink = rgb565(255, 120, 160);
        draw_heart(cx - 24, cy - 40, 8, pink);
        draw_heart(cx + 22, cy - 48, 6, pink);
        draw_heart(cx,      cy - 58, 7, pink);
    }
    // Poke reward: sparkles burst when a poke run completes.
    if (sparkTimer_ > 0.0f) {
        const uint16_t spark = rgb565(255, 230, 80);
        fb.fillCircle(cx - 26, cy - 30, 3, spark);
        fb.fillCircle(cx + 26, cy - 34, 3, spark);
        fb.fillCircle(cx + 4,  cy - 52, 3, spark);
    }

    // status markers
    if (p.sick)      gfx_text(cx - 20, cy - 42, 2, col::warn, "SICK");
    if (p.lightsOff) gfx_text(cx + 20, cy - 36, 2, col::white, "Zzz");

    // HUD panel
    fb.fillRect(0, 0, GAME_W, 62, col::panel);
    gfx_text(6, 6, 2, col::accent, "%s", pet.displayName());

    gfx_text(6, 30, 1, col::white, "HUN");
    gfx_bar(38, 29, 80, 9, p.hunger / 100.0f, bar_color(p.hunger / 100.0f), col::black, col::dim);
    gfx_text(126, 30, 1, col::white, "HAP");
    gfx_bar(158, 29, 74, 9, p.happiness / 100.0f, bar_color(p.happiness / 100.0f), col::black, col::dim);
    gfx_text(6, 46, 1, col::white, "HP");
    gfx_bar(38, 45, 80, 9, p.health / 100.0f, bar_color(p.health / 100.0f), col::black, col::dim);

    int batpct = (int)((BAT_analogVolts - 3.0f) / (4.2f - 3.0f) * 100.0f);
    if (batpct < 0) batpct = 0;
    if (batpct > 100) batpct = 100;
    gfx_text(126, 46, 1, col::white, "BAT %d%%", batpct);

    datetime_t dt = clock_datetime();   // consistent snapshot (no torn h:m:s)
    gfx_text(6, HORIZON + 12, 1, col::white, "%02d:%02d:%02d",
             dt.hour, dt.minute, dt.second);

    // bottom action bar: quick care actions
    for (int i = 0; i < ACT_N; i++) {
        int bx = act_x(i);
        bool feedBlocked = (i == 0 && (p.sick || p.lightsOff));   // no feeding while sick/asleep
        uint16_t bg = feedBlocked ? rgb565(46, 42, 46) : col::panel;
        fb.fillRoundRect(bx, ACT_Y, ACT_W, ACT_H, 7, bg);
        int ix = bx + ACT_W / 2, iy = ACT_Y + 18;
        switch (i) {
            case 0: icon_feed(ix, iy, feedBlocked); break;
            case 1: icon_clean(ix, iy); break;
            case 2: icon_heal(ix, iy); break;
            case 3: icon_lights(ix, iy, p.lightsOff, bg); break;
        }
        const char* lbl = (i == 3) ? (p.lightsOff ? "Wake" : "Sleep") : ACT_LABEL[i];
        int lw = (int)strlen(lbl) * 6;
        gfx_text(bx + (ACT_W - lw) / 2, ACT_Y + ACT_H - 12, 1,
                 feedBlocked ? col::dim : col::white, "%s", lbl);
    }

    // top-right menu button ("other" activities/settings/stats)
    fb.fillRoundRect(MB_X, MB_Y, MB_W, MB_H, 6, col::panel);
    for (int k = 0; k < 3; k++)
        fb.fillRect(MB_X + 8, MB_Y + 6 + k * 6, MB_W - 16, 3, col::white);

    // runtime debug overlay (toggle in Settings): touch internals + sim state.
    // Drawn over the sky just below the HUD (above the pet) so it clips nothing.
    if (app().debugOverlay) {
        fb.fillRect(0, 64, GAME_W, 38, rgb565(0, 0, 0));
        gfx_text(4, 66, 1, col::good, "down%d over%d act%d xy%d,%d",
                 down_, overPet_, touchActive_, tx_, ty_);
        gfx_text(4, 78, 1, col::good, "prog%.0f/%d dist%.0f mv%.1f",
                 rubProgress_, (int)PET_CHUNK_DIST, rubDist_, lastMove_);
        gfx_text(4, 90, 1, col::good, "hr%d slp%d stg%.0f%% spd%ux hun%.0f hp%.0f",
                 pet.simHour(), pet.isSleepTime(), pet.stageProgress() * 100.0f,
                 (unsigned)p.gameSpeed, p.hunger, p.health);
    }
}

void SceneHome::onInput(const Input& in)
{
    down_ = in.down;
    tx_ = in.x;
    ty_ = in.y;
    // Creature poke/pet gestures are resolved in update(); here we handle the taps
    // on the top-right menu button and the bottom action bar.
    if (!in.pressed) return;
    if (hit(in.x, in.y, MB_HX, MB_HY, MB_HW, MB_HH)) { app().setScene(SceneId::Menu, Slide::Forward); return; }
    for (int i = 0; i < ACT_N; i++) {
        if (hit(in.x, in.y, act_x(i), ACT_Y, ACT_W, ACT_H)) {
            switch (i) {
                case 0: app().pet.feed();         break;
                case 1: app().pet.clean();        break;
                case 2: app().pet.heal();         break;
                case 3: app().pet.toggleLights(); break;
            }
            return;
        }
    }
}
