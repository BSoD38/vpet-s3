#include "scene_home.hpp"
#include "core/app.hpp"
#include "sim/items.hpp"       // Item (the toy that is out)
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

// The toy sits on the ground at a fixed spot near the left edge, out of the walk lane's
// middle so the creature does not permanently stand on top of it. World coordinates: it is
// part of the ROOM, so it pans and zooms with the pinch camera like the grass does.
static const float TOY_WX = 40.0f;
static const int   TOY_R  = 13;
static const float TOY_PLAY_TIME = 0.45f;
// Footfall period while running a ball down, as a fraction of the walking one. Halving it
// doubles both the leg speed and the ground speed, which is what "running" has to mean here.
static const float BALL_RUN_MULT = 0.5f;

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
// Camera follow (docs/camera.md): per-second convergence of the view center onto the
// creature (~90% of the way in 0.45 s) -- fast enough that a 4x zoom never loses the pet,
// slow enough that its stops and turns read as a camera trailing it, not bolted to it.
static const float CAM_FOLLOW_RATE = 5.0f;
// Runtime debug overlay (Settings toggle): a black band of size-1 text under the HUD.
// Its height and the floor it pushes the creature's cues down to are derived from ONE
// line count. They used to be two hand-synced magic numbers, and adding a line to the
// band without bumping the floor leaves the speech bubble painted UNDER it while its hit
// rect keeps eating taps -- an invisible target that warps you into the conversation.
static const int   DBG_Y     = 48;
static const int   DBG_LINES = 5;
static const int   DBG_H     = DBG_LINES * 12 + 2;   // 12px per row, +2 bottom padding

static uint16_t bar_color(float v01)
{
    return v01 < 0.30f ? col::warn : col::good;
}


// A small stacked "poop" with a dark outline so it reads against the brown ground.
// World-space (drawn through the camera): a poop is a thing lying on the ground, so it
// pans and scales with it. Int params on purpose -- the halved radii below must keep
// their integer-division values so the identity camera renders it pixel-identically.
static void draw_poop(const Camera2D& cam, int x, int y)
{
    const uint16_t body = rgb565(74, 46, 20);
    const uint16_t edge = rgb565(30, 18, 8);
    gfx_fill_circle_world(cam, x, y,      8, body);
    gfx_fill_circle_world(cam, x, y - 7,  6, body);
    gfx_fill_circle_world(cam, x, y - 13, 4, body);
    gfx_draw_circle_world(cam, x, y,      8, edge);
    gfx_draw_circle_world(cam, x, y - 7,  6, edge);
}

// World-space too: hearts hang around the creature's body, so they follow its zoom.
static void draw_heart(const Camera2D& cam, int x, int y, int s, uint16_t c)
{
    gfx_fill_circle_world(cam, x - s / 2, y, s / 2, c);
    gfx_fill_circle_world(cam, x + s / 2, y, s / 2, c);
    gfx_fill_triangle_world(cam, x - s, y + s / 4, x + s, y + s / 4, x, y + s + s / 3, c);
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
    if (toyPlayT_ > 0.0f) toyPlayT_ -= dt;
    Pet& pet = app().pet;
    const PetState& p = pet.state();

    t_ += dt;
    if (pokeTarget_ == 0) pokeTarget_ = 3 + (int)(esp_random() % 4);   // this run needs 3..6 pokes

    // Camera gesture FIRST: the frame a second finger lands, engaged() must already be
    // true here -- to the single-finger tracking below a second finger looks like the
    // touch point teleporting, which the rub accumulator would bank as petting distance.
    // While it is engaged, any armed gesture is dropped and none may re-arm.
    pinch_.update(in_, cam_);
    // TWO FINGERS DOWN IS ENOUGH, engaged or not: the recognizer deliberately refuses to
    // engage while the points are closer than MIN_PINCH_DIST, and in exactly that window
    // (the start of every pinch-out, and the one-finger-plus-ghost case) tx_/ty_ still
    // come from whichever contact the driver put in slot 0 -- so it can jump between the
    // two. This is the same predicate onInput() gates taps on; the two must agree, or one
    // half of the scene treats the touch as a pinch while the other half rubs the pet.
    const bool pinching = pinch_.engaged() || in_.points >= 2;
    if (pinching) { touchActive_ = false; ringHold_ = 0.0f; }

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
    // The camera trails the creature's STABLE anchor -- walk x + baseline body center,
    // deliberately without the footfall lift / poke hop / refusal wiggle, so those animate
    // the pet within the frame instead of shaking the whole world with it. (walk_.x() is
    // last frame's position here, same as the petting zone below: 0.4 px at walk speed,
    // and the follow smoothing swallows far more than that.)
    cam_.follow(walk_.x(), (float)bodyCy, dt, CAM_FOLLOW_RATE);
    int hzW = psw / 2 + PET_HIT_MARGIN, hzH = psh / 2 + PET_HIT_MARGIN;
    // The zone is WORLD-space (it is the creature's body), the finger is SCREEN-space:
    // map the finger into the world before testing. At identity this is a no-op.
    bool onPetZone = Rect{ bodyCx - hzW, bodyCy - hzH, hzW * 2, hzH * 2 }
                         .contains((int)lroundf(cam_.wx(tx_)), (int)lroundf(cam_.wy(ty_)));
    const bool frozen = pet.frozen();           // care freeze: nothing here reaches the creature
    bool interactive = p.stage != STAGE_EGG && !p.lightsOff && !pet.touchBlocked() && !frozen;
    bool overPet = interactive && onPetZone;    // can actually pet/poke
    overPet_ = overPet;
    // THE one test for "this touch is the creature's to answer". Two things can take it
    // away: a pinch (whose stray leftover finger must stay inert), and a press the HUD has
    // already consumed -- the petting zone is WORLD space, so once zoomed in it projects
    // out over the screen-space action bar, and without this a single tap down there would
    // both press a button and poke the pet. Every consumer below goes through it, so a new
    // one can't quietly miss a case the way three parallel `!pinching` guards invited.
    // The toy takes the touch before the creature does. The finger is mapped INTO world space,
    // the same way the petting zone does it above, so nothing here needs zoom scaling.
    const char* outToyId = app().room.toy();
    const int   outToy   = (outToyId && outToyId[0]) ? app().items.indexOf(outToyId) : -1;
    const bool  tossToy  = outToy >= 0 && app().items.at(outToy).play == PLAY_TOSS;

    // A ball has to be placed the first time it is put out, and re-placed if the toy changes.
    if (tossToy && !ballReady_) { ball_.place(TOY_WX); ballReady_ = true; }
    if (!tossToy && ballReady_) { ballReady_ = false; }

    if (outToy >= 0 && !pinching) {
        const Item& toy = app().items.at(outToy);
        const float fwx = cam_.wx(tx_), fwy = cam_.wy(ty_);

        if (tossToy) {
            // Drag and flick. The grab keeps the finger-to-ball offset so the ball does not
            // jump to the fingertip, and holding it keeps the touch consumed for the whole
            // gesture -- otherwise releasing over the creature would also poke it.
            if (down_ && !wasDown_ && !tapConsumed_ && ball_.contains(fwx, fwy, (float)PET_FEET)) {
                ball_.grab(fwx, fwy, (float)PET_FEET);
                tapConsumed_ = true;
            } else if (down_ && ball_.held) {
                ball_.drag(fwx, fwy, (float)PET_FEET, dt);
                tapConsumed_ = true;
            } else if (!down_ && ball_.held) {
                ball_.release();
                tapConsumed_ = true;
            }
        } else if (down_ && !wasDown_ && !tapConsumed_) {
            const float dx = fwx - TOY_WX, dy = fwy - ((float)PET_FEET - TOY_R);
            const float r  = TOY_R + 6.0f;                 // slack: it is a small target
            if (dx * dx + dy * dy <= r * r) {
                if (pet.playWithToy(toy.drift, toy.happiness)) {
                    toyPlayT_ = TOY_PLAY_TIME;
                    anim_.react(Anim::Happy, TOY_PLAY_TIME);
                } else {
                    refuseTimer_ = REFUSE_TIME;
                }
                tapConsumed_ = true;
            }
        }
    }

    const bool touchFree = !pinching && !tapConsumed_;
    bool justPressed  = down_ && !wasDown_ && touchFree;
    bool justReleased = !down_ && wasDown_;

    // Pose layers: sim state drives the persistent base; short-lived events overlay it.
    anim_.tick(dt);
    // Any ailment (sick, injured, convalescing) shows the Sick pose -- there is no per-
    // condition art, and "unwell" is the message that matters at a glance.
    anim_.setBase(p.lightsOff ? Anim::Nap : pet.conditionBlocked() ? Anim::Sick : Anim::Idle);
    // Aged gait (docs/death-and-lifespan.md §4): stretching the footfall period is the one
    // lever that slows the walk coherently -- ground travel derives from the step phase
    // (engine/walk.hpp), so gait and speed slow together. Presentation only, by design.
    // Kept as a named base rather than written straight into the animation, because the ball
    // chase further compresses it below. Multiplying the LIVE value there instead would only
    // be correct while this happened to run first, and would collapse to the floor the moment
    // the two were reordered.
    float gaitBase;
    {
        LifeTrack lt = pet.lifeTrack();
        const VitalsTuning& T = vitals_tuning();
        gaitBase = ANIM_STEP_SECS * (lt == LIFE_TWILIGHT ? T.twilightStepMult
                                   : lt == LIFE_ELDERLY  ? T.elderlyStepMult : 1.0f);
        anim_.setStepSecs(gaitBase);
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
        // arm a gesture only if the touch begins on the creature (and it accepts touch);
        // touchFree above has already excluded pinches and presses the HUD took
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
    bool touched    = touchActive_ || (touchFree && down_ && overPet);
    bool stepping   = anim_.walkCycle();
    bool travelling = p.stage != STAGE_EGG && !touched && !pet.isUpset() && !frozen;
    // --- ball play ---------------------------------------------------------------------
    // The creature runs a ball down, bats it, and goes after it again. Chasing takes over the
    // wander director's DIRECTION only; the speed comes from compressing the footfall period,
    // so the legs move as fast as the body does (see walk.hpp).
    bool chasing = false;
    if (ballReady_ && ball_.live && travelling && !ball_.held) {
        ball_.update(dt, walk_.minSpan(), walk_.maxSpan());
        if (ball_.live) {
            chasing = true;
            walk_.chase(ball_.x);
            // Bat it away once close enough AND it has settled -- swiping at a ball still
            // flying past would let the creature volley it without ever running for it.
            if (ball_.withinReach(walk_.x())) {
                ball_.batFrom(walk_.x());
                hopTimer_ = POKE_HOP_TIME;
                sfx::play(sfx::kPet, 0.5f);
                // Metered: a rally is dozens of bats, and paying drift for each would make a
                // ball the fastest personality farm in the game.
                if (ballRewardCd_ <= 0.0f) {
                    const int ti = app().items.indexOf(app().room.toy());
                    if (ti >= 0 && pet.playWithToy(app().items.at(ti).drift,
                                                   app().items.at(ti).happiness)) {
                        anim_.react(Anim::Happy, POKE_HOP_TIME);
                        ballRewardCd_ = PLAY_COOLDOWN;
                    }
                }
            }
        }
    } else if (ballReady_ && !ball_.held) {
        ball_.update(dt, walk_.minSpan(), walk_.maxSpan());
    }
    if (!chasing) walk_.clearTarget();
    if (ballRewardCd_ > 0.0f) ballRewardCd_ -= dt;

    // Running compresses the footfall period, from the gait base rather than from whatever
    // the period happens to be this frame.
    anim_.setStepSecs(chasing ? gaitBase * BALL_RUN_MULT : gaitBase);

    walk_.update(dt, stepping, travelling, anim_.stepPhase(), anim_.stepSecs());
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

    // background -- WORLD drawing from here down to the HUD panel: everything goes through
    // cam_ (docs/camera.md), so a pinch zoom scales and pans it while the HUD holds still.
    // With the camera at identity these render exactly as they did before it existed.
    gfx_fill_rect_world(cam_, 0, 0, GAME_W, HORIZON, col::sky);
    // ground: a row of grass tiles at the horizon, dirt tiles filling below
    gfx_tile_region_world(cam_, 0, HORIZON, GAME_W, TILE_H, grass_tile, TILE_W, TILE_H);
    gfx_tile_region_world(cam_, 0, HORIZON + TILE_H, GAME_W, GAME_H - HORIZON - TILE_H,
                          dirt_tile, TILE_W, TILE_H);

    // poop blobs on the ground
    for (int i = 0; i < p.poop; i++)
        draw_poop(cam_, 34 + i * 44, HORIZON + 34);

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
    // The toy that is out, drawn on the ground BEFORE the creature so it walks in front of
    // it. Primitives from the item colour rather than a sprite: costs no art, and swaps for
    // one later without touching any of this logic (docs/economy-and-inventory.md 6).
    const char* toyId = app().room.toy();
    const int   toyIdx = (toyId && toyId[0]) ? app().items.indexOf(toyId) : -1;
    if (toyIdx >= 0 && app().items.at(toyIdx).play == PLAY_TOSS && ballReady_) {
        ball_.draw(cam_, (float)PET_FEET, app().items.at(toyIdx).color);
    } else if (toyIdx >= 0) {
        const Item& toy = app().items.at(toyIdx);
        const float bounce = toyPlayT_ > 0.0f ? -10.0f * (toyPlayT_ / TOY_PLAY_TIME) : 0.0f;
        const float ty = (float)PET_FEET - TOY_R + bounce;
        switch (toy.shape) {
            case SHAPE_SQUARE:
            case SHAPE_SOFT:
                gfx_fill_rect_world(cam_, TOY_WX - TOY_R, ty - TOY_R,
                                    TOY_R * 2.0f, TOY_R * 2.0f, toy.color);
                break;
            case SHAPE_BAR:
                gfx_fill_rect_world(cam_, TOY_WX - TOY_R, ty - TOY_R * 0.45f,
                                    TOY_R * 2.0f, TOY_R * 0.9f, toy.color);
                break;
            default:
                gfx_fill_circle_world(cam_, TOY_WX, ty, (float)TOY_R, toy.color);
                gfx_draw_circle_world(cam_, TOY_WX, ty, (float)TOY_R, col::black);
                break;
        }
    }

    if (spr) gfx_blit_sprite_bottom_world(cam_, spr, cx, feet, SPRITE_TRANSP, anim_.mirrored());
    else     gfx_blit_world(cam_, SPR_FALLBACK, cx, feet - SPRITE_H / 2);   // 48px "?" fallback, feet-anchored

    // Rub-progress ring: shown while ringHold_ is armed (a live rub, plus its tail -- update()).
    // Pink fill = progress to the next happiness chunk. The fill colour is CONSTANT: it used
    // to dim on any frame the finger wasn't moving, and since a rub has micro-pauses (plus a
    // 1px deadzone) that read as a fast flicker rather than as feedback.
    if (ringHold_ > 0.0f) {
        gfx_fill_arc_world(cam_, cx, cy, 30, 35, 0, 360, rgb565(50, 52, 62));
        float p = rubProgress_ / PET_CHUNK_DIST;
        if (p > 1.0f) p = 1.0f;
        if (p > 0.0f)
            gfx_fill_arc_world(cam_, cx, cy, 30, 35, 0, (int)(p * 360.0f), rgb565(255, 120, 160));
    }

    // Petting reward: hearts linger for the cooldown.
    if (playCooldown_ > 0.0f) {
        const uint16_t pink = rgb565(255, 120, 160);
        draw_heart(cam_, cx - 24, cy - 40, 8, pink);
        draw_heart(cam_, cx + 22, cy - 48, 6, pink);
        draw_heart(cam_, cx,      cy - 58, 7, pink);
    }
    // Poke reward: sparkles burst when a poke run completes.
    if (sparkTimer_ > 0.0f) {
        const uint16_t spark = rgb565(255, 230, 80);
        gfx_fill_circle_world(cam_, cx - 26, cy - 30, 3, spark);
        gfx_fill_circle_world(cam_, cx + 26, cy - 34, 3, spark);
        gfx_fill_circle_world(cam_, cx + 4,  cy - 52, 3, spark);
    }

    // The cues from here down are "nameplates": they belong to the creature but stay at a
    // fixed SCREEN size (the text renderer only does integer sizes, and a 2x-zoomed badge
    // would just be blur) -- so they hang off the creature's PROJECTED position and keep
    // today's pixel offsets. At identity these equal cx / cy / the head top exactly.
    int scx = (int)lroundf(cam_.sx((float)cx));           // creature center, screen space
    int scy = (int)lroundf(cam_.sy((float)cy));
    int shd = (int)lroundf(cam_.sy((float)(feet - psh))); // head top, screen space

    // status markers ("SICK"/"VERY SICK"/"HURT"/... -- see condition_marker)
    if (const char* cm = pet.conditionMarker())
        gfx_text(clamp_left(scx - 20, (int)strlen(cm) * 12), scy - 42, 2, col::warn, "%s", cm);
    if (p.lightsOff) gfx_text(clamp_left(scx + 20, 36), scy - 36, 2, col::white, "Zzz");

    // Speech bubble: the creature has something to say. Deliberately PERSISTENT -- it doesn't
    // time out and vanish, so the payoff for a high bond can't be missed by looking away.
    // Sits to the RIGHT of the head (the attention badge takes the left), and the rect is
    // stashed for onInput since it moves with the sprite's height.
    // The debug overlay (drawn last) owns the band below the HUD when enabled; the bubble
    // and the badge must clamp BELOW it or they'd be painted over while their hit-rects
    // kept eating taps.
    const int cueMinY = app().debugOverlay ? DBG_Y + DBG_H + 4 : DBG_Y;

    // Hidden while frozen as well as while asleep: a conversation moves bond, mood and
    // personality, so offering one would contradict the pause. It keeps its place and is
    // still waiting on the way out.
    bubble_ = Rect{ 0, 0, 0, 0 };
    if (app().conversations.pending() && !p.lightsOff && !frozen) {
        int by = shd - 26;
        if (by < cueMinY) by = cueMinY;            // never ride up into the HUD panel/overlay
        Rect b{ scx + 24, by, 46, 30 };
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
        int by = shd - 10;                         // above the head, whatever the sprite size
        if (by < cueMinY + 6) by = cueMinY + 6;    // ...but never riding up into the HUD/overlay
        int bx = clamp_centre(scx - 44, 14);       // beside the head, but never off the edge
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
        fb.fillRect(0, DBG_Y, GAME_W, DBG_H, rgb565(0, 0, 0));
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
        // camera + multitouch internals (read next to the FPS number app.cpp draws)
        gfx_text(4, 98, 1, col::good, "cam z%.2f p%d xy2 %d,%d %s",
                 cam_.zoom(), in_.points, in_.x2, in_.y2,
                 pinch_.engaged() ? "PINCH" : "");
    }
}

void SceneHome::onInput(const Input& in)
{
    in_ = in;      // full snapshot for the pinch recognizer (fed to it in update())
    down_ = in.down;
    tx_ = in.x;
    ty_ = in.y;
    // The HUD's claim on a press lasts exactly as long as that press. Cleared on the way
    // up rather than on the next press edge, because the press debounce can swallow a
    // quick re-tap's edge -- which would otherwise inherit the previous tap's verdict.
    if (!in.down) tapConsumed_ = false;
    // A pinch owns the touch outright: while two fingers are down -- and through the tail
    // of the gesture, until the LAST finger lifts -- no tap may fire. (What this can't
    // catch: a pinch whose first finger lands on a button, because that press-edge tap has
    // already fired before a second finger exists. Pinches start over the scene in
    // practice, so that stays accepted rather than adding a tap delay to every button.)
    if (in.points >= 2 || pinch_.engaged()) return;
    // Creature poke/pet gestures are resolved in update(); here we handle the taps
    // on the top-right menu button and the bottom action bar.
    if (!in.pressed) return;
    // Record that this press belongs to a screen-space control before dispatching it --
    // update() needs to know, because its petting zone is world-space and at zoom can
    // cover the same pixels (see touchFree there). Recomputed at every press edge, so it
    // never outlives the press that set it.
    tapConsumed_ = (app().conversations.pending() && bubble_.w > 0 && bubble_.contains(in))
                || MENU_HIT.contains(in);
    for (int i = 0; i < ACT_N && !tapConsumed_; i++)
        if (act_rect(i).contains(in)) tapConsumed_ = true;
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
