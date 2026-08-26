#include "scene_home.hpp"
#include "core/app.hpp"
#include "engine/gfx.hpp"
#include "engine/util.hpp"      // clampf (shared)
#include "ui/widgets.hpp"
#include "sim/creatures.hpp"        // Creature (current sprite)
#include "engine/clock.hpp"            // clock_datetime (torn-read-safe RTC snapshot)
#include "engine/battery.hpp"          // battery_state (gauge + charger detection)
#include "assets/sprites.hpp"   // spr_unknown_data (fallback sprite)
#include "assets/tiles.hpp"     // grass_tile / dirt_tile (ground)
#include "esp_random.h"
#include <cmath>
#include <cstdio>
#include <cstring>

static const Sprite SPR_FALLBACK { spr_unknown_data, SPRITE_W, SPRITE_H, SPRITE_TRANSP };  // "?" when a sprite can't be shown

static const int HORIZON = GAME_H * 2 / 3;
// Baseline the creature's feet rest on. gfx_blit_sprite_bottom puts the sprite's LAST ROW at
// bottomY-1, and the grass row starts at HORIZON, so +2 overlaps the feet two rows into the
// grass and reads as planted. This used to be HORIZON-3, leaving a 4px band of sky under the
// feet: invisible while the old bob swung +-4px through the ground line, but the footfall bob
// only ever lifts UP, so the gap became a permanent hover. Anything that changes the bob's
// sign has to be checked against this constant.
static const int PET_FEET = HORIZON + 2;
static const int PET_HIT_MARGIN = 8;        // petting-zone slack around the sprite's box
// The creature is no longer pinned to the centre: it walks the ground between these insets
// (measured to the sprite's EDGE, so wide creatures get the same clearance as narrow ones).
static const int WALK_MARGIN = 6;

// bottom action bar (quick care actions) + top-right menu button
static const int   ACT_N  = 4;
static const int   ACT_W  = 52, ACT_H = 52, ACT_GAP = 5, ACT_X0 = 8, ACT_Y = 260;
static const int   MB_X   = 204, MB_Y = 4, MB_W = 32, MB_H = 24;   // visible button
// HUD row 2, split by what it is about: the CREATURE on the left (the HP gauge) and the
// DEVICE on the right (battery, then the clock hard against the right margin, under the menu
// button it lines up with). The gap between the two groups is the point -- it keeps a glance
// at the pet's health from landing on the clock. The clock used to sit on the grass and the
// battery on the name row; moving both here gives the name the whole of row 1 and clears the
// creature's sky of text.
static const int   ROW2_Y = 30;                 // top of the row-2 text and the HP bar
static const int   HP_X   = 6;                  // "HP" label
static const int   HPB_X  = 24, HPB_W = 72;     // the gauge itself (no number: the bar IS the
                                                // readout, and exact health is on the Stats sheet)
static const int   CLK_X  = 204;                // "HH:MM" at size 1 is 30px -> ends at x234
// Battery body + terminal nub run BAT_X..BAT_X+25, one pixel taller than the text either
// side, so it is seated a row above them to share a centre line.
static const int   BAT_X  = 170, BAT_Y = ROW2_Y - 1, BAT_W = 22, BAT_H = 11;
// The charging bolt sits in the gap to the LEFT of the icon rather than inside the 18x7 fill
// area, where it would have needed an outline to stay legible over both the filled and the
// empty half. The gap is kept clear WHETHER OR NOT the bolt is showing -- a layout that
// reflowed every time the charger came and went would be worse than one empty notch.
static const int   BOLT_X = BAT_X - 10, BOLT_Y = BAT_Y + 5;
// The name now has row 1 to itself, up to the menu button's touch strip.
static const int   NAME_W = 182;
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
// The rub ring keeps drawing for a moment after the rub conditions lapse. They can lapse
// for a frame or two in the middle of a perfectly good rub -- the finger crosses the hit
// zone's edge, or the touch controller drops a poll -- and hiding it the instant they do
// read as a flicker. Long enough to bridge that, short enough that letting go still feels
// like letting go.
static const float RING_LINGER     = 0.30f;   // rub ring lingers this long once the rub stops
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
// Idle flourish played on some of the wander director's stops (see update()). Kept well under
// half so most stops are just a stop -- these are punctuation, not the main motion.
static const int   IDLE_POSE_PCT   = 25;      // % of stops that strike a pose
static const float IDLE_POSE_TIME  = 1.1f;
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
// Lightning bolt, centred on (x,y): the "a charger is attached" marker. The board gives the
// MCU no charge-status line, so this is driven by engine/battery.cpp reading the plug-in out
// of the voltage -- see there for what that can and cannot see.
static void icon_bolt(int x, int y, uint16_t c)
{
    fb.fillTriangle(x + 2, y - 5, x - 3, y + 1, x + 1, y + 1, c);
    fb.fillTriangle(x - 2, y + 5, x + 3, y - 1, x - 1, y - 1, c);
}

// Battery as a status-bar glyph: outlined body + terminal nub, with a fill whose width and
// colour track the charge (green / amber / red). Reads at a glance and costs a fifth of the
// width the old "BAT nn%" text did; the exact figure lives in the debug overlay.
static void icon_battery(int x, int y, int pct, bool charging)
{
    const uint16_t fgc = charging     ? col::good
                       : pct <= 20    ? col::warn
                       : pct <= 50    ? rgb565(235, 150, 60)
                                      : col::good;
    fb.drawRoundRect(x, y, BAT_W, BAT_H, 2, col::white);
    fb.fillRect(x + BAT_W, y + BAT_H / 2 - 2, 3, 5, col::white);   // terminal nub
    int inner = BAT_W - 4;                                         // 2px inset inside the frame
    int fill  = (inner * (pct < 0 ? 0 : pct) + 50) / 100;          // pct < 0 = no reading yet
    if (fill > 0) fb.fillRect(x + 2, y + 2, fill, BAT_H - 4, fgc);
    if (charging) icon_bolt(BOLT_X, BOLT_Y, col::accent);
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

    // Petting zone tracks the creature's actual sprite size (feet-anchored baseline) and
    // FOLLOWS it along the ground -- otherwise you would be rubbing empty grass. Still no
    // bob applied: the box stays vertically stable so a rub can't be shaken off it. The x
    // is last frame's (the walk is stepped at the end of update, once every reaction that
    // could stop it is known), which at ~18 px/s is a fifth of a pixel behind.
    LGFX_Sprite* petSpr = app().creatures.sprite(pet.creatureIndex());
    int psw = petSpr ? petSpr->width()  : SPRITE_W;
    int psh = petSpr ? petSpr->height() : SPRITE_H;
    walk_.setSpan(WALK_MARGIN + psw / 2, GAME_W - WALK_MARGIN - psw / 2);
    int bodyCx = (int)walk_.x();
    int bodyCy = PET_FEET - psh / 2;
    int hzW = psw / 2 + PET_HIT_MARGIN, hzH = psh / 2 + PET_HIT_MARGIN;
    bool onPetZone = Rect{ bodyCx - hzW, bodyCy - hzH, hzW * 2, hzH * 2 }.contains(tx_, ty_);
    const bool frozen = pet.frozen();           // care freeze: nothing here reaches the creature
    bool interactive = p.stage != STAGE_EGG && !p.lightsOff && !pet.touchBlocked() && !frozen;
    bool overPet = interactive && onPetZone;    // can actually pet/poke
    overPet_ = overPet;
    bool justPressed  = down_ && !wasDown_;
    bool justReleased = !down_ && wasDown_;

    // Pose layers: sim state drives the persistent base; short-lived events overlay it.
    anim_.tick(dt);
    // Any ailment (sick, injured, convalescing) shows the Sick pose -- there is no per-
    // condition art, and "unwell" is the message that matters at a glance.
    anim_.setBase(p.lightsOff ? Anim::Nap : pet.conditionBlocked() ? Anim::Sick : Anim::Idle);
    // Aged gait (docs/death-and-lifespan.md §4): stretching the footfall period is the one
    // lever that slows the walk coherently -- ground travel derives from the step phase
    // (engine/walk.hpp), so gait and speed slow together. Presentation only, by design.
    {
        LifeTrack lt = pet.lifeTrack();
        const VitalsTuning& T = vitals_tuning();
        anim_.setStepSecs(ANIM_STEP_SECS * (lt == LIFE_TWILIGHT ? T.twilightStepMult
                                          : lt == LIFE_ELDERLY  ? T.elderlyStepMult : 1.0f));
    }
    if (pet.checkAte()) anim_.react(Anim::Eat, 2.4f);   // fed from SceneFeed; play it on arrival

    // "no" wiggle: refused from the menu (e.g. feed while sick), or from touching a sick pet
    auto refuse = [&] { refuseTimer_ = REFUSE_TIME; anim_.react(Anim::Nope, REFUSE_TIME); };
    if (pet.checkRefused()) refuse();
    // Sick, or paused: either way the touch goes nowhere, and a shake says so. (Pet::play()
    // would arm the same wiggle, but overPet is already false in both cases, so it is never
    // reached -- this is the branch that actually answers a finger on the creature.)
    if (justPressed && onPetZone && (pet.touchBlocked() || frozen)) refuse();
    // An UPSET creature won't be touched (PetMood): refuse at first contact and never arm a
    // gesture, so no rub ring appears and no bond can be farmed from a refusing creature.
    // (Food is still accepted -- that's the way back.)
    if (justPressed && overPet && pet.isUpset()) refuse();

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
            if (playCooldown_ <= 0.0f) {                     // not on cooldown
                rubProgress_ += eff;
                if (rubProgress_ >= PET_CHUNK_DIST) {        // earned a chunk
                    rubProgress_ = 0.0f;
                    // Bond and hearts only when the touch was ACCEPTED: a refusal (e.g. the
                    // creature turned upset mid-rub) must grant nothing.
                    if (pet.play(PET_CHUNK, PLAY_AFFECTION)) {
                        pet.addFriendship(FR_PET);
                        playCooldown_ = PLAY_COOLDOWN;        // hearts linger for the cooldown
                        ringHold_ = 0.0f;                     // hearts replace the ring at once
                        anim_.react(Anim::Happy, PLAY_COOLDOWN);
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
            anim_.react(Anim::Happy, POKE_HOP_TIME);
            if (++pokeCount_ >= pokeTarget_) {  // run complete -> reward + full lockout
                if (pet.play((float)POKE_CHUNK, PLAY_ROUGH)) {   // refused = no bond, no sparkles
                    pet.addFriendship(FR_POKE);
                    sparkTimer_ = PLAY_COOLDOWN; // sparkles linger for the whole cooldown
                    anim_.react(Anim::Happy, PLAY_COOLDOWN);
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

    // Rub ring: armed while the rub is live, then held for RING_LINGER once it isn't, so a
    // momentary lapse in these conditions can't blink it off. Read by render(), which no
    // longer tests them itself.
    if (touchActive_ && overPet && playCooldown_ <= 0.0f && rubDist_ > POKE_MAX_DIST)
        ringHold_ = RING_LINGER;

    // Locomotion. Stepped LAST so every reaction armed above is already visible to
    // anim_.walkCycle(), which is true only while the two-frame walk pose is on screen --
    // that one test covers asleep, sick, eating, refusing and the petting-reward wiggle, so
    // none of them needs naming here. What is left is the cases where the walk cycle IS
    // showing but travelling would be wrong:
    //   - an egg, which has no legs to walk on;
    //   - a finger on the creature, so it can't stroll out from under a rub;
    //   - an upset creature (PetMood), which sulks on the spot. It already refuses to be
    //     touched, and standing still says that better than pacing would.
    //   - a FROZEN creature. Its clock isn't running, so it has nowhere to walk to; holding
    //     position (while the idle frames keep flipping, so it doesn't read as crashed) is
    //     the clearest possible statement that the world is paused.
    // The two are separate because showing the walk pose and being allowed to cover ground
    // are different questions: the first is the animation's business, the second the scene's.
    bool touched    = touchActive_ || (down_ && overPet);
    bool stepping   = anim_.walkCycle();
    bool travelling = p.stage != STAGE_EGG && !touched && !pet.isUpset() && !frozen;
    walk_.update(dt, stepping, travelling, anim_.stepPhase());
    anim_.face(walk_.facingRight());   // DMC sprites face left, so mirror == walking right

    // Idle flourish: when the wander director stops the creature, it sometimes strikes a pose
    // before moving on. Most stops stay plain on purpose -- the walk-cycle bob keeps it alive
    // while it stands, and something that emotes at every single stop reads as twitchy rather
    // than alive. Mood-appropriate, so even the flavour carries care information rather than
    // being noise: a grump if it wants something, a pleased bounce if it doesn't. Safe to
    // reuse Happy (the petting reward pose) -- hearts are drawn off playCooldown_, so there
    // is no false reward signal. The reaction stops the walk cycle for its duration, which
    // freezes the director, so the rest simply resumes when the pose is done.
    if (walk_.takeRestCue() && (int)(esp_random() % 100) < IDLE_POSE_PCT) {
        int ht = care_tier(p.hunger), mt = care_tier(p.happiness);
        anim_.react((ht < mt ? ht : mt) <= CARE_TIER_NEEDY ? Anim::Angry : Anim::Happy,
                    IDLE_POSE_TIME);
    }

    prevx_ = tx_; prevy_ = ty_; wasDown_ = down_;
    if (hopTimer_ > 0)     hopTimer_ -= dt;
    if (sparkTimer_ > 0)   sparkTimer_ -= dt;
    if (pokeCd_ > 0)       pokeCd_ -= dt;
    if (playCooldown_ > 0) playCooldown_ -= dt;
    if (refuseTimer_ > 0)  refuseTimer_ -= dt;
    if (ringHold_ > 0)     ringHold_ -= dt;
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

// Cues hung off the creature (badge, mood cloud, SICK/Zzz text) used to sit at a fixed
// centre and were always safely on screen. Now that the creature walks to both edges they
// have to be clamped, or they slide off with it.
static int clamp_left(int x, int w)   // left-anchored (text): keep [x, x+w) on screen
{
    if (x < 2) return 2;
    return (x + w > GAME_W - 2) ? GAME_W - 2 - w : x;
}
static int clamp_centre(int x, int r)  // centre-anchored (glyphs of radius r)
{
    if (x < r) return r;
    return (x > GAME_W - r) ? GAME_W - r : x;
}

// Care-freeze marker: a pause glyph on a cool disc. Takes the badge slot and OUTRANKS both the
// mood cloud and the attention prompt -- while the sim is suspended neither of those is
// actionable, and "nothing is running" is the one thing the player has to be able to read.
// Deliberately static (no pulse): everything else in that slot animates, and stillness is the
// message. kFrozenCol is shared with the Settings row, the Menu hint and the Stats sheet.
static void draw_frozen(int x, int y)
{
    fb.fillCircle(x, y, 10, kFrozenCol);
    fb.drawCircle(x, y, 10, col::black);
    fb.fillRect(x - 4, y - 5, 3, 11, col::black);
    fb.fillRect(x + 2, y - 5, 3, 11, col::black);
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
    const bool frozen = pet.frozen();

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
    int cx = (int)walk_.x() + wig;
    int hop = hopTimer_ > 0.0f ? -(int)(12.0f * (hopTimer_ / POKE_HOP_TIME)) : 0;
    // baseline + the footfall bob + poke hop. walk_.lift() is the ONLY idle vertical motion:
    // it completes exactly one rise-and-fall per animation frame, is phase-locked to it, and
    // is 0 whenever the creature isn't actually covering ground -- so a standing creature
    // holds its height while its walk frames keep alternating on the spot. There used to be a
    // second, free-running "breathing" bob added on top, and because its period had nothing to
    // do with the cadence the two slid in and out of phase -- which made the walk look wrong.
    int feet = PET_FEET + walk_.lift() + hop;
    // Current pose frame (idle flip / nap / sick / reactions); single-pose creatures
    // (the placeholder line, the egg) return their one sprite for any frame index.
    LGFX_Sprite* spr = app().creatures.frame(pet.creatureIndex(), anim_.frame());   // lazy-loaded + cached
    int psh = spr ? spr->height() : SPRITE_H;
    int cy = feet - psh / 2;                                    // body center (for ring/hearts/markers)
    if (spr) gfx_blit_sprite_bottom(spr, cx, feet, SPRITE_TRANSP, anim_.mirrored());
    else     gfx_blit(SPR_FALLBACK, cx, feet - SPRITE_H / 2);   // 48px "?" fallback, feet-anchored

    // Rub-progress ring: shown while ringHold_ is armed (a live rub, plus its tail -- update()).
    // Pink fill = progress to the next happiness chunk. The fill colour is CONSTANT: it used
    // to dim on any frame the finger wasn't moving, and since a rub has micro-pauses (plus a
    // 1px deadzone) that read as a fast flicker rather than as feedback.
    if (ringHold_ > 0.0f) {
        fb.fillArc(cx, cy, 30, 35, 0, 360, rgb565(50, 52, 62));
        float p = rubProgress_ / PET_CHUNK_DIST;
        if (p > 1.0f) p = 1.0f;
        if (p > 0.0f)
            fb.fillArc(cx, cy, 30, 35, 0, (int)(p * 360.0f), rgb565(255, 120, 160));
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

    // status markers ("SICK"/"VERY SICK"/"HURT"/... -- see condition_marker)
    if (const char* cm = pet.conditionMarker())
        gfx_text(clamp_left(cx - 20, (int)strlen(cm) * 12), cy - 42, 2, col::warn, "%s", cm);
    if (p.lightsOff) gfx_text(clamp_left(cx + 20, 36), cy - 36, 2, col::white, "Zzz");

    // Speech bubble: the creature has something to say. Deliberately PERSISTENT -- it doesn't
    // time out and vanish, so the payoff for a high bond can't be missed by looking away.
    // Sits to the RIGHT of the head (the attention badge takes the left), and the rect is
    // stashed for onInput since it moves with the sprite's height.
    // The debug overlay (drawn last) owns y48..98 when enabled; the bubble and the badge
    // must clamp BELOW it or they'd be painted over while their hit-rects kept eating taps.
    const int cueMinY = app().debugOverlay ? 102 : 48;

    // Hidden while frozen as well as while asleep: a conversation moves bond, mood and
    // personality, so offering one would contradict the pause. It keeps its place and is
    // still waiting on the way out.
    bubble_ = Rect{ 0, 0, 0, 0 };
    if (app().conversations.pending() && !p.lightsOff && !frozen) {
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
    // The freeze marker shares the slot but ignores the asleep/egg guard: those states are
    // themselves suspended, so the pause is the more important thing to say about either.
    if (frozen || (!p.lightsOff && p.stage != STAGE_EGG)) {
        int by = feet - psh - 10;                  // above the head, whatever the sprite size
        if (by < cueMinY + 6) by = cueMinY + 6;    // ...but never riding up into the HUD/overlay
        int bx = clamp_centre(cx - 44, 14);        // beside the head, but never off the edge
        if (frozen) {
            draw_frozen(bx, by);
        } else if (pet.isUpset()) {
            draw_mood(bx, by, t_, pet.mood() == MOOD_ANGRY);
        } else {
            int ht = care_tier(p.hunger), mt = care_tier(p.happiness);
            int worst = ht < mt ? ht : mt;
            if (worst <= CARE_TIER_NEEDY)
                draw_attention(bx, by, t_, care_tier_color(worst));
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
    // The name is player-set, so it's fitted to the row rather than trusted to be short (a
    // 16-char nickname used to run under the menu button).
    gfx_text_fit(6, 6, NAME_W, 2, col::accent, "%s", pet.displayName());

    gfx_text(HP_X, ROW2_Y, 1, col::white, "HP");
    gfx_bar(HPB_X, ROW2_Y - 1, HPB_W, 9, p.health / 100.0f,
            bar_color(p.health / 100.0f), col::black, col::dim);

    // Straight from the shared gauge: engine/battery.cpp already filters the ADC and rate-
    // limits the percentage, so there is nothing left here for a scene-local latch to steady.
    const BatteryState bat = battery_state();
    icon_battery(BAT_X, BAT_Y, bat.pct, bat.charging);

    datetime_t dt = clock_datetime();   // consistent snapshot (no torn h:m)
    gfx_text(CLK_X, ROW2_Y, 1, col::white, "%02d:%02d", dt.hour, dt.minute);

    // Caption for the freeze, above the (fully greyed) action bar. Says where to undo it:
    // Settings is three taps away and there is no other route back.
    if (frozen) {
        const char* msg = "Care paused - Settings > Game";
        int mw = (int)strlen(msg) * 6;
        gfx_text((GAME_W - mw) / 2, ACT_Y - 13, 1, kFrozenCol, "%s", msg);
    }

    // bottom action bar: quick care actions
    for (int i = 0; i < ACT_N; i++) {
        int bx = act_x(i);
        // Frozen greys the WHOLE bar: every one of these refuses, and a button that looks
        // live but only ever answers with the "no" wiggle is worse than an honest one.
        bool blocked = frozen || (i == 0 && (pet.foodBlocked() || p.lightsOff));   // no feeding while sick/asleep
        uint16_t bg = blocked ? rgb565(46, 42, 46) : col::panel;
        act_rect(i).fill(bg, 7);
        int ix = bx + ACT_W / 2, iy = ACT_Y + 18;
        switch (i) {
            case 0: icon_feed(ix, iy, blocked); break;
            case 1: icon_clean(ix, iy); break;
            case 2: icon_heal(ix, iy); break;
            case 3: icon_lights(ix, iy, p.lightsOff, bg); break;
        }
        const char* lbl = (i == 3) ? (p.lightsOff ? "Wake" : "Sleep") : ACT_LABEL[i];
        int lw = (int)strlen(lbl) * 6;
        gfx_text(bx + (ACT_W - lw) / 2, ACT_Y + ACT_H - 12, 1,
                 blocked ? col::dim : col::white, "%s", lbl);
    }

    // top-right menu button ("other" activities/settings/stats)
    fb.fillRoundRect(MB_X, MB_Y, MB_W, MB_H, 6, col::panel);
    for (int k = 0; k < 3; k++)
        fb.fillRect(MB_X + 8, MB_Y + 6 + k * 6, MB_W - 16, 3, col::white);

    // runtime debug overlay (toggle in Settings): touch internals + sim state.
    // Drawn over the sky just below the HUD (above the pet) so it clips nothing.
    if (app().debugOverlay) {
        fb.fillRect(0, 48, GAME_W, 50, rgb565(0, 0, 0));
        gfx_text(4, 50, 1, col::good, "down%d over%d act%d xy%d,%d w%d%s",
                 down_, overPet_, touchActive_, tx_, ty_,
                 (int)walk_.x(), !walk_.walking() ? "." : walk_.facingRight() ? ">" : "<");
        gfx_text(4, 62, 1, col::good, "prog%.0f/%d dist%.0f mv%.1f %.2fV%s",
                 rubProgress_, (int)PET_CHUNK_DIST, rubDist_, lastMove_, bat.volts,
                 bat.charging ? "+" : "-");
        // exact care values live here now that the HUD shows coarse states. The speed field
        // doubles as the freeze readout -- the line is already the full 240px wide, and a
        // frozen sim has no meaningful multiplier to report.
        char sp[10];
        if (frozen) strcpy(sp, "FROZEN");
        else        snprintf(sp, sizeof sp, "spd%ux", (unsigned)p.gameSpeed);
        gfx_text(4, 74, 1, col::good, "hr%d slp%d stg%.0f%% %s hun%.0f hap%.0f hp%.0f",
                 pet.simHour(), pet.isSleepTime(), pet.stageProgress() * 100.0f,
                 sp, p.hunger, p.happiness, p.health);
        // life meter internals (docs/death-and-lifespan.md): pool, trailing neglect
        // average, condition + minutes it has held (drives escalation)
        gfx_text(4, 86, 1, col::good, "vit%.0f ema%.2f cond%u t%.0fm",
                 pet.vitals().vitality, pet.vitals().careEma,
                 p.cond, pet.vitals().condSecs / 60.0f);
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
                // feeding immediately. canEat() arms the "no" wiggle when it can't eat; the
                // sound has to be asked for explicitly, because pre-gating here means feed()
                // -- which is what normally voices a refusal -- is never reached.
                case 0: if (app().pet.canEat()) app().setScene(SceneId::Feed, Slide::Forward);
                        else                    app().pet.playRefusal();
                        break;
                case 1: app().pet.clean();        break;
                case 2: app().setScene(SceneId::Medicine, Slide::Forward); break;
                case 3: app().pet.toggleLights(); break;
            }
            return;
        }
    }
}
