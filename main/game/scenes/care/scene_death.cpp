#include "scene_death.hpp"
#include "core/app.hpp"
#include "engine/gfx.hpp"
#include "engine/audio/sfx.hpp"
#include "sim/creatures.hpp"     // frame indices (FRM_*)
#include "sim/lineage.hpp"       // generation number for the memorial
#include "assets/sprites.hpp"    // SPRITE_TRANSP
#include "ui/widgets.hpp"
#include <cstdio>
#include <cstring>

// Phase lengths. The first three are not skippable -- about seven seconds the player
// simply sits through, which is the point: the event is witnessed, not clicked past.
static const float DUSK_S     = 2.0f;
static const float COLLAPSE_S = 2.2f;
static const float STILL_S    = 2.6f;
static const float HINT_AFTER = 1.6f;   // "(tap)" appears this long into the verdict

static const int GROUND_Y = GAME_H * 2 / 3;
static const Rect kAgain { 40, 272, GAME_W - 80, 34 };

// The passing, after a death farewell (conversation deaths only; the plain path has its
// own collapse). The conversation already carried the creature to the centre at the fate
// crossing, so this begins exactly where that left off -- same spot, same box -- and
// simply lets it sleep.
static const float PASS_NAP_S   = 5.4f;   // asleep, held -- long enough to really watch it
static const float PASS_FADE_S  = 2.4f;   // the frame darkens to black (gfx_fade overlay)
static const float MEM_FADE_S   = 1.0f;   // and the memorial rises out of the same black
static const float DEPART_S     = 4.0f;   // "Begin again": one long breath out, then the torch passes
static const int   PASS_BOX     = 76;     // == the conversation's PORTRAIT_BOX

static const uint16_t kNightSky    = rgb565(7, 8, 14);
static const uint16_t kNightGround = rgb565(16, 15, 20);

// Centred single line of text (size-1 cell is 6 px, size-2 is 12).
static void ctext(int y, int size, uint16_t c, const char* s)
{
    gfx_text((GAME_W - (int)strlen(s) * 6 * size) / 2, y, size, c, "%s", s);
}

// A small headstone for the memorial: rounded stone, darker outline, two rest lines.
static void draw_stone(int cx, int y)
{
    const uint16_t stone = rgb565(96, 100, 112), edge = rgb565(40, 42, 52);
    fb.fillRoundRect(cx - 16, y, 32, 34, 9, stone);
    fb.fillRect(cx - 22, y + 28, 44, 6, stone);
    fb.drawRoundRect(cx - 16, y, 32, 34, 9, edge);
    fb.fillRect(cx - 9, y + 12, 18, 2, edge);
    fb.fillRect(cx - 9, y + 18, 18, 2, edge);
}

void SceneDeath::onEnter()
{
    toPhase(Ph::Dusk);
    saidGoodbye_ = false;
    haveConv_    = false;
    fadeInMem_   = false;
    // Old-age deaths look for a deathbed farewell (D3); Critical deaths stay wordless BY
    // DESIGN -- neglect takes the pet before anything can be said, and that contrast is
    // the point (grief with words, guilt without). afterConv_ survives onEnter: it marks
    // a RETURN from the farewell, consumed by the first update below.
    searching_ = (app().pet.brink() == BRINK_OLDAGE) && !afterConv_;
}

void SceneDeath::update(float dt)
{
    Pet& pet = app().pet;

    // Returning from the farewell: everything was said in dialogue, fate included. Resolve
    // straight to the outcome -- update runs before render, so no Dusk frame flashes.
    if (afterConv_) {
        afterConv_ = false;
        if (pet.fate() == FATE_MIRACLE) {
            pet.applyMiracle();
            app().setScene(SceneId::Home, Slide::None);
        } else {
            toPhase(Ph::Passing);   // the last words are said; now it goes to sleep
        }
        return;
    }

    // The farewell search rides under the prologue, a couple of files per frame -- seven
    // silent seconds hide even a large modded library's walk.
    if (searching_ && app().conversations.triggeredStep("deathbed", app().convCtx()))
    {
        searching_ = false;
        haveConv_  = app().conversations.pending();
    }

    t_ += dt;
    switch (ph_) {
        case Ph::Dusk:
            if (t_ >= DUSK_S) toPhase(Ph::Collapse);
            break;
        case Ph::Collapse:
            if (!saidGoodbye_) {
                saidGoodbye_ = true;
                sfx::play(sfx::kSad);        // its own voice, one last time
            }
            if (t_ >= COLLAPSE_S) toPhase(Ph::Still);
            break;
        case Ph::Still:
            // Also held until the search settles -- with a sane library it long since has.
            if (t_ >= STILL_S && !searching_) {
                if (haveConv_) {
                    // The farewell IS the rest of the event: the creature has things to
                    // say, and fate reveals itself mid-dialogue (the @fate fork).
                    app().conversationScene.returnTo(SceneId::Death);
                    app().setScene(SceneId::Conversation, Slide::None);
                } else {
                    toPhase(Ph::Verdict);
                }
            }
            break;
        case Ph::Passing:
            // No effects here -- not even the sleep cue (its descending tone read as a
            // pratfall). The farewell music is the only sound the passing makes; the
            // darkening itself is drawn by render() (gfx_fade).
            if (t_ >= PASS_NAP_S + PASS_FADE_S) {
                fadeInMem_ = true;           // the memorial rises out of the same black
                toPhase(Ph::Memorial);
            }
            break;
        case Ph::Memorial:
            if (fadeInMem_ && t_ >= MEM_FADE_S) fadeInMem_ = false;
            break;
        case Ph::Depart:
            if (t_ >= DEPART_S) app().restartAfterDeath();   // does not return
            break;
        default:                             // Verdict waits on the player
            break;
    }
}

void SceneDeath::render()
{
    Pet& pet = app().pet;
    const bool oldage  = pet.brink() == BRINK_OLDAGE;
    const bool miracle = pet.fate()  == FATE_MIRACLE;

    if (ph_ == Ph::Passing) {
        // Still the conversation's room: same panel colour, and the creature is already at
        // the centre (the fate crossing carried it there). The world does NOT change
        // around it -- it just sleeps, and then the frame darkens. Only behind that
        // darkness does the memorial's night exist; the fade IS the scene change.
        fb.fillScreen(col::panel);
        int frm = (((int)(t_ / 1.2f)) & 1) ? FRM_NAP2 : FRM_NAP1;
        LGFX_Sprite* spr = app().creatures.frame(pet.creatureIndex(), frm);
        if (spr) gfx_blit_sprite_fit(spr, GAME_W / 2, GAME_H / 2, PASS_BOX, PASS_BOX, SPRITE_TRANSP);
        if (t_ > PASS_NAP_S) gfx_fade((t_ - PASS_NAP_S) / PASS_FADE_S);
        return;
    }

    // Night, everywhere. No HUD, no clock, no battery: nothing else is happening.
    fb.fillScreen(kNightSky);
    fb.fillRect(0, GROUND_Y, GAME_W, GAME_H - GROUND_Y, kNightGround);

    if (ph_ == Ph::Memorial || ph_ == Ph::Depart) {
        // Who they were. The pet object is still loaded -- concludeDeath() runs on the
        // way out -- so this reads straight from the life that just ended.
        char line[64];
        ctext(46, 2, col::dim, "In memory");
        draw_stone(GAME_W / 2, 74);

        ctext(126, 2, col::white, pet.displayName());
        if (pet.nickname()[0]) {             // named: the species goes underneath
            snprintf(line, sizeof line, "the %s", pet.speciesName());
            ctext(146, 1, col::dim, line);
        }
        snprintf(line, sizeof line, "Generation %u", (unsigned)lineage_generation(app().save));
        ctext(170, 1, col::dim, line);
        char age[16];
        snprintf(line, sizeof line, "Lived %s", pet.ageStr(age, sizeof age));
        ctext(186, 1, col::dim, line);
        snprintf(line, sizeof line, "%s, to the end", pet.friendshipTier());
        ctext(202, 1, col::accent, line);

        // An egg is waiting. Naming that softens the button without cheapening the page.
        ctext(246, 1, col::dim, "An egg is waiting.");
        kAgain.button("Begin again", col::accent, col::black);
        // Reached through the passing's black: the page rises out of the same darkness --
        // and leaves the same way once "Begin again" is chosen, music and light together.
        if (ph_ == Ph::Depart)   gfx_fade(t_ / DEPART_S);
        else if (fadeInMem_)     gfx_fade(1.0f - t_ / MEM_FADE_S);
        return;
    }

    // The creature, centre stage. It stands through the dusk, sinks in the collapse, and
    // lies still -- and if the roll said miracle, the verdict phase has it back on its
    // feet before a word is shown: the interruption is VISIBLE, not narrated.
    int frm = FRM_IDLE1;
    if      (ph_ == Ph::Collapse) frm = (t_ < COLLAPSE_S * 0.5f) ? FRM_SICK : FRM_LOSE;
    else if (ph_ == Ph::Still)    frm = FRM_LOSE;
    else if (ph_ == Ph::Verdict)  frm = miracle ? FRM_IDLE1 : FRM_LOSE;

    LGFX_Sprite* spr = app().creatures.frame(pet.creatureIndex(), frm);
    if (spr) gfx_blit_sprite_bottom(spr, GAME_W / 2, GROUND_Y + 2, SPRITE_TRANSP, false);

    if (ph_ == Ph::Verdict) {
        char l1[48];
        const char* l2;
        if (miracle) {
            if (oldage) { snprintf(l1, sizeof l1, "%s holds on,", pet.displayName());
                          l2 = "a little longer."; }
            else        { snprintf(l1, sizeof l1, "%s refused", pet.displayName());
                          l2 = "to leave you."; }
        } else {
            if (oldage) { snprintf(l1, sizeof l1, "%s let go,", pet.displayName());
                          l2 = "gently, at the end of a long life."; }
            else        { snprintf(l1, sizeof l1, "%s couldn't", pet.displayName());
                          l2 = "hold on."; }
        }
        ctext(64, 2, col::white, l1);
        ctext(86, 1, col::white, l2);
        // High bond earns one more line -- the deathbed conversations (D3) will replace
        // this with real dialogue; until then the game still acknowledges what it saw.
        if (!miracle && pet.friendship() >= 6000)
            ctext(104, 1, col::accent, "It stayed as long as it could, for you.");

        if (t_ >= HINT_AFTER) ctext(GAME_H - 26, 1, col::dim, "(tap)");
    }
}

void SceneDeath::onInput(const Input& in)
{
    if (!in.pressed) return;

    if (ph_ == Ph::Verdict) {
        Pet& pet = app().pet;
        if (pet.fate() == FATE_MIRACLE) {
            // The reprieve/save applies on the tap that acknowledges it; back to the world.
            pet.applyMiracle();
            app().setScene(SceneId::Home, Slide::None);
        } else {
            toPhase(Ph::Memorial);
        }
        return;
    }
    if (ph_ == Ph::Memorial && kAgain.contains(in)) {
        sfx::play(sfx::kSelect);
        audio::music_stop(DEPART_S - 0.3f);  // the farewell theme breathes out with the light
        toPhase(Ph::Depart);                 // update() restarts the chip when the black lands
    }
    // Dusk/Collapse/Still/Depart: the event is witnessed, not clicked past. Taps do nothing.
}
