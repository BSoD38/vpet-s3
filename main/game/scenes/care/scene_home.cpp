#include "scene_home.hpp"
#include "core/app.hpp"
#include "engine/gfx.hpp"
#include "engine/util.hpp"      // clampf (shared)
#include "ui/widgets.hpp"
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
// Battery icon, top row: right-aligned into the gap between the name and the menu button
// (body + terminal nub end at x185, clear of MENU_HIT below), vertically centered on the
// size-2 name text.
static const int   BAT_X  = 160, BAT_Y = 9, BAT_W = 22, BAT_H = 11;
// The gauge is latched on this interval instead of following the ADC. The driver already
// filters the reading (drivers/BAT_Driver), but a battery is a slow thing and the icon is
// coarse -- 18px of fill, so ~5% per pixel -- and a level that moves at all on its own reads
// as broken. Anything under a minute of drift is invisible at this size.
static const float BAT_REFRESH = 15.0f;   // seconds between re-reads
// Touch target = the top-right corner strip ABOVE the stat bars (bars start at
// y29). Kept generous in x but capped so that even with TOUCH_SLOP the hit box tops out at
// y28 (h + slop = 27 + 2) and never reaches the HAP bar.
static const Rect  MENU_HIT { 192, 0, GAME_W - 192, 27 };          // tap here -> open Menu
static const char* ACT_LABEL[4] = { "Feed", "Clean", "Heal", "Sleep" };
static int  act_x(int i)    { return ACT_X0 + i * (ACT_W + ACT_GAP); }
static Rect act_rect(int i) { return { act_x(i), ACT_Y, ACT_W, ACT_H }; }

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

// Li-ion 3.0V (empty) .. 4.2V (full), linear -- crude, but the icon has 18px of fill.
static int battery_pct()
{
    return (int)clampf((BAT_analogVolts - 3.0f) / (4.2f - 3.0f) * 100.0f, 0.0f, 100.0f);
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
// Battery as a status-bar glyph: outlined body + terminal nub, with a fill whose width and
// colour track the charge (green / amber / red). Reads at a glance and costs a fifth of the
// width the old "BAT nn%" text did; the exact figure lives in the debug overlay.
static void icon_battery(int x, int y, int pct)
{
    const uint16_t fgc = pct <= 20 ? col::warn
                       : pct <= 50 ? rgb565(235, 150, 60)
                                   : col::good;
    fb.drawRoundRect(x, y, BAT_W, BAT_H, 2, col::white);
    fb.fillRect(x + BAT_W, y + BAT_H / 2 - 2, 3, 5, col::white);   // terminal nub
    int inner = BAT_W - 4;                                         // 2px inset inside the frame
    int fill  = (inner * pct + 50) / 100;
    if (fill > 0) fb.fillRect(x + 2, y + 2, fill, BAT_H - 4, fgc);
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

    // Battery: sampled on a slow timer (immediately on the first frame), so the gauge steps
    // between settled readings instead of tracking the ADC.
    batTimer_ -= dt;
    if (batPct_ < 0 || batTimer_ <= 0.0f) {
        batTimer_ = BAT_REFRESH;
        batPct_   = battery_pct();
    }

    // Petting zone tracks the creature's actual sprite size (feet-anchored baseline).
    LGFX_Sprite* petSpr = app().creatures.sprite(pet.creatureIndex());
    int psw = petSpr ? petSpr->width()  : SPRITE_W;
    int psh = petSpr ? petSpr->height() : SPRITE_H;
    int bodyCy = PET_FEET - psh / 2;              // no bob here: keep the hit box stable
    int hzW = psw / 2 + PET_HIT_MARGIN, hzH = psh / 2 + PET_HIT_MARGIN;
    bool onPetZone = Rect{ PET_CX - hzW, bodyCy - hzH, hzW * 2, hzH * 2 }.contains(tx_, ty_);
    bool interactive = p.stage != STAGE_EGG && !p.lightsOff && !p.sick;
    bool overPet = interactive && onPetZone;    // can actually pet/poke
    overPet_ = overPet;
    rubbingNow_ = false;
    bool justPressed  = down_ && !wasDown_;
    bool justReleased = !down_ && wasDown_;

    // "no" wiggle: refused from the menu (e.g. feed while sick), or from touching a sick pet
    if (pet.checkRefused()) refuseTimer_ = REFUSE_TIME;
    if (justPressed && onPetZone && p.sick) refuseTimer_ = REFUSE_TIME;
    // An UPSET creature won't be touched (PetMood): refuse at first contact and never arm a
    // gesture, so no rub ring appears and no bond can be farmed from a refusing creature.
    // (Food is still accepted -- that's the way back.)
    if (justPressed && overPet && pet.isUpset()) refuseTimer_ = REFUSE_TIME;

    if (justPressed) {
        // arm a gesture only if the touch begins on the creature (and it accepts touch)
        touchActive_ = overPet && !pet.isUpset();
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
                    // Bond and hearts only when the touch was ACCEPTED: a refusal (e.g. the
                    // creature turned upset mid-rub) must grant nothing.
                    if (pet.play(PET_CHUNK, PLAY_AFFECTION)) {
                        pet.addFriendship(FR_PET);
                        playCooldown_ = PLAY_COOLDOWN;        // hearts linger for the cooldown
                    }
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
                if (pet.play((float)POKE_CHUNK, PLAY_ROUGH)) {   // refused = no bond, no sparkles
                    pet.addFriendship(FR_POKE);
                    sparkTimer_ = PLAY_COOLDOWN; // sparkles linger for the whole cooldown
                }
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

// Attention badge: a small pulsing "!" disc that prompts the player to go and LOOK at the
// creature. Now that hunger and mood are off the HUD, nothing else would tell them to.
// It is a prompt, not a gauge -- it never says what is wrong or by how much, only that
// something is. Deliberately NOT tappable: it sits over the petting zone, so a hit-test
// here would swallow rub/poke gestures.
// Badge appears from CARE_TIER_NEEDY down (Hungry / Glum) -- i.e. BEFORE things get dire,
// because creature expression is meant to carry the extreme states later on. The constant
// is shared with the conversation context's "hungry" gate (sim/pet.hpp) so a modded
// conversation about hunger can only fire while the badge is actually showing.

// Upset marker: a small rain-cloud when hurt, a red steam puff when angry. Takes the same slot
// as the attention badge and wins it -- a creature that won't be touched is more urgent news
// than a hunger reminder, and two badges side by side just reads as clutter.
static void draw_mood(int x, int y, float phase, bool angry)
{
    if (angry) {
        const uint16_t r = rgb565(230, 80, 70);
        fb.fillCircle(x - 5, y, 6, r);
        fb.fillCircle(x + 5, y, 6, r);
        fb.fillCircle(x, y - 4, 7, r);
        for (int i = 0; i < 3; i++) {                 // steam, rising
            int dy = (int)(4.0f * sinf(phase * 4.0f + i));
            fb.fillCircle(x - 8 + i * 8, y - 13 + dy, 2, r);
        }
    } else {
        const uint16_t g = rgb565(120, 130, 155);
        fb.fillCircle(x - 6, y - 2, 6, g);
        fb.fillCircle(x + 6, y - 2, 6, g);
        fb.fillCircle(x, y - 6, 7, g);
        for (int i = 0; i < 2; i++) {                 // drizzle
            int dy = ((int)(phase * 12.0f) + i * 5) % 10;
            fb.fillRect(x - 5 + i * 10, y + 4 + dy, 2, 4, rgb565(90, 140, 200));
        }
    }
}

static void draw_attention(int x, int y, float phase, uint16_t color)
{
    int r = 9 + (int)(2.0f * sinf(phase * 5.0f));
    fb.fillCircle(x, y, r, color);
    fb.drawCircle(x, y, r, col::black);
    fb.fillRect(x - 1, y - 6, 3, 7, col::black);   // stroke
    fb.fillRect(x - 1, y + 3, 3, 3, col::black);   // dot
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

    // Speech bubble: the creature has something to say. Deliberately PERSISTENT -- it doesn't
    // time out and vanish, so the payoff for a high bond can't be missed by looking away.
    // Sits to the RIGHT of the head (the attention badge takes the left), and the rect is
    // stashed for onInput since it moves with the sprite's height.
    // The debug overlay (drawn last) owns y48..86 when enabled; the bubble and the badge
    // must clamp BELOW it or they'd be painted over while their hit-rects kept eating taps.
    const int cueMinY = app().debugOverlay ? 90 : 48;

    bubble_ = Rect{ 0, 0, 0, 0 };
    if (app().conversations.pending() && !p.lightsOff) {
        int by = feet - psh - 26;
        if (by < cueMinY) by = cueMinY;            // never ride up into the HUD panel/overlay
        Rect b{ cx + 24, by, 46, 30 };
        if (b.x + b.w > GAME_W - 4) b.x = GAME_W - 4 - b.w;
        bubble_ = b;
        b.fill(col::white, 8);
        b.outline(col::accent, 8);
        fb.fillTriangle(b.x + 8, b.y + b.h - 1, b.x + 20, b.y + b.h - 1,
                        b.x + 6, b.y + b.h + 8, col::white);       // tail toward the creature
        // Three dots, animated, so it reads as "talking" rather than as a static badge.
        for (int i = 0; i < 3; i++) {
            int dy = ((int)(t_ * 3.0f) % 3 == i) ? -2 : 0;
            fb.fillCircle(b.x + 13 + i * 10, b.y + b.h / 2 + dy, 3, rgb565(60, 66, 92));
        }
    }

    // Attention prompt: the only thing that now tells the player to go and look, since
    // hunger and mood left the HUD. Covers just those two -- sickness, sleep and poop
    // already have their own visible markers above. Suppressed while asleep (feeding is
    // refused then, so there would be nothing to act on) and for an egg (it can't eat).
    // Sits left of centre so it never collides with the SICK / Zzz text.
    if (!p.lightsOff && p.stage != STAGE_EGG) {
        int by = feet - psh - 10;                  // above the head, whatever the sprite size
        if (by < cueMinY + 6) by = cueMinY + 6;    // ...but never riding up into the HUD/overlay
        if (pet.isUpset()) {
            draw_mood(cx - 44, by, t_, pet.mood() == MOOD_ANGRY);
        } else {
            int ht = care_tier(p.hunger), mt = care_tier(p.happiness);
            int worst = ht < mt ? ht : mt;
            if (worst <= CARE_TIER_NEEDY)
                draw_attention(cx - 44, by, t_, care_tier_color(worst));
        }
    }

    // HUD panel. Hunger and mood are deliberately NOT shown here: even a coarse readout
    // sitting permanently on screen is something players watch and optimise, and the whole
    // point of the care redesign is that you read the CREATURE instead
    // (docs/conversations-and-personality.md 2.8). Both remain on the Stats sheet, for a
    // deliberate check-in rather than an ambient gauge, and exact values are available
    // there and in the debug overlay below via Settings > Debug info.
    // The panel is sized to what's left (name + HP + battery), so no empty band remains.
    fb.fillRect(0, 0, GAME_W, 44, col::panel);
    // The name is player-set, so it's fitted to the space left of the battery icon rather
    // than trusted to be short (a 16-char nickname used to run under the menu button).
    gfx_text_fit(6, 6, BAT_X - 12, 2, col::accent, "%s", pet.displayName());

    icon_battery(BAT_X, BAT_Y, batPct_);   // update() seeds this before the first render

    gfx_text(6, 28, 1, col::white, "HP");
    gfx_bar(26, 27, 72, 9, p.health / 100.0f, bar_color(p.health / 100.0f), col::black, col::dim);
    gfx_text(102, 28, 1, col::dim, "%d", (int)p.health);

    datetime_t dt = clock_datetime();   // consistent snapshot (no torn h:m:s)
    gfx_text(6, HORIZON + 12, 1, col::white, "%02d:%02d:%02d",
             dt.hour, dt.minute, dt.second);

    // bottom action bar: quick care actions
    for (int i = 0; i < ACT_N; i++) {
        int bx = act_x(i);
        bool feedBlocked = (i == 0 && (p.sick || p.lightsOff));   // no feeding while sick/asleep
        uint16_t bg = feedBlocked ? rgb565(46, 42, 46) : col::panel;
        act_rect(i).fill(bg, 7);
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
        fb.fillRect(0, 48, GAME_W, 38, rgb565(0, 0, 0));
        gfx_text(4, 50, 1, col::good, "down%d over%d act%d xy%d,%d",
                 down_, overPet_, touchActive_, tx_, ty_);
        gfx_text(4, 62, 1, col::good, "prog%.0f/%d dist%.0f mv%.1f bat%.2fV",
                 rubProgress_, (int)PET_CHUNK_DIST, rubDist_, lastMove_, BAT_analogVolts);
        // exact care values live here now that the HUD shows coarse states
        gfx_text(4, 74, 1, col::good, "hr%d slp%d stg%.0f%% spd%ux hun%.0f hap%.0f hp%.0f",
                 pet.simHour(), pet.isSleepTime(), pet.stageProgress() * 100.0f,
                 (unsigned)p.gameSpeed, p.hunger, p.happiness, p.health);
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
    // Checked before the pet gestures: tapping the bubble must open the conversation, not
    // register as a poke. Handling it here (onInput runs before update) means the gesture
    // logic never sees the touch, because the scene has already changed.
    if (app().conversations.pending() && bubble_.w > 0 && bubble_.contains(in)) {
        app().setScene(SceneId::Conversation, Slide::Forward);
        return;
    }
    if (MENU_HIT.contains(in)) { app().setScene(SceneId::Menu, Slide::Forward); return; }
    for (int i = 0; i < ACT_N; i++) {
        if (act_rect(i).contains(in)) {
            switch (i) {
                // Feeding is now a choice of food, so this opens the picker rather than
                // feeding immediately. canEat() arms the "no" wiggle when it can't eat,
                // so a blocked tap still gives the same feedback it always did.
                case 0: if (app().pet.canEat()) app().setScene(SceneId::Feed, Slide::Forward);
                        break;
                case 1: app().pet.clean();        break;
                case 2: app().pet.heal();         break;
                case 3: app().pet.toggleLights(); break;
            }
            return;
        }
    }
}
