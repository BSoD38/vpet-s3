#include "scene_stats.hpp"
#include "core/app.hpp"
#include "engine/gfx.hpp"
#include "ui/widgets.hpp"
#include "assets/sprites.hpp"   // spr_unknown_data (fallback)
#include <cstdio>

static const Sprite SPR_FALLBACK { spr_unknown_data, SPRITE_W, SPRITE_H, SPRITE_TRANSP };  // "?" when a sprite can't be shown

// buttons (this screen's Back sits bottom-right beside Rename, not the shared top-right slot)
static const Rect RN_BTN { 16,  274, 100, 34 };   // Rename
static const Rect BK_BTN { 124, 274, 100, 34 };   // Back

static void draw_heart(int x, int y, int s, uint16_t c)
{
    fb.fillCircle(x - s / 2, y, s / 2, c);
    fb.fillCircle(x + s / 2, y, s / 2, c);
    fb.fillTriangle(x - s, y + s / 4, x + s, y + s / 4, x, y + s + s / 3, c);
}

void SceneStats::render()
{
    Pet& pet = app().pet;
    const PetState& p = pet.state();
    fb.fillScreen(col::panel);

    // --- identity (thumbnail scaled to fit the header box) ---
    LGFX_Sprite* spr = app().creatures.sprite(pet.creatureIndex());
    if (spr) gfx_blit_sprite_fit(spr, 40, 44, 56, 56, SPRITE_TRANSP);
    else     gfx_blit(SPR_FALLBACK, 40, 44);   // "?" when the real sprite can't be drawn
    gfx_text(78, 12, 2, col::white, "%s", pet.displayName());
    gfx_text(78, 38, 1, col::dim, "the %s", pet.speciesName());
    char ab[16];
    gfx_text(78, 52, 1, col::dim, "%s   %s", pet.stageName(), pet.ageStr(ab, sizeof ab));
    // Who the creature has BECOME, from how it's been raised. Named, but the axis values
    // behind it stay hidden: identity should be legible, steering it shouldn't be.
    char plbl[40];
    gfx_text(78, 62, 1, col::accent, "%s", app().drift.label(plbl, sizeof plbl));

    fb.drawFastHLine(12, 72, GAME_W - 24, col::dim);

    // --- care meters ---
    // Hunger and mood are named STATES, not numbers: a precise gauge is something players
    // optimise, and everyone optimising it the same way flattens personality drift (see
    // docs/conversations-and-personality.md 2.8). HP and Energy keep exact bars -- they are
    // explicit game resources (battle gating, training costs) where precision is fairness.
    gfx_text(12, 78, 1, col::accent, "CARE");
    // The sheet is where a returning player checks what state they left things in, so it has
    // to say when the numbers below are frozen rather than merely unchanged.
    if (pet.frozen()) gfx_text(52, 78, 1, kFrozenCol, "- PAUSED");
    gfx_text(12, 92, 1, col::white, "Hunger");
    gfx_text(70, 92, 1, care_tier_color(care_tier(p.hunger)), "%s", hunger_label(p.hunger));
    gfx_text(12, 105, 1, col::white, "Mood");
    gfx_text(70, 105, 1, care_tier_color(care_tier(p.happiness)), "%s", mood_label(p.happiness));
    if (app().debugOverlay)      // exact values on demand, for testing
        gfx_text(160, 92, 1, col::dim, "%d/%d", (int)p.hunger, (int)p.happiness);

    const char* clab[2] = { "HP", "ENE" };
    float cval[2] = { p.health, p.energy };
    for (int i = 0; i < 2; i++) {
        int y = 118 + i * 13;
        gfx_text(12, y + 1, 1, col::white, "%s", clab[i]);
        float f = cval[i] / 100.0f;
        uint16_t fg = (i == 1) ? (f < 0.30f ? col::warn : rgb565(90, 170, 255))   // ENE = stamina (blue)
                               : (f < 0.30f ? col::warn : col::good);
        gfx_bar(46, y, 148, 9, f, fg, col::black, col::dim);
        gfx_text(200, y + 1, 1, col::dim, "%d", (int)cval[i]);
    }

    fb.drawFastHLine(12, 140, GAME_W - 24, col::dim);

    // --- battle stats (effective = creature base + trained modifier) ---
    gfx_text(12, 146, 1, col::accent, "BATTLE");
    gfx_text(140, 146, 1, col::dim, "W:%u L:%u", (unsigned)pet.wins(), (unsigned)pet.losses());
    gfx_text(12, 164, 1, col::dim, "MAX HP");
    gfx_text(78, 160, 2, col::white, "%u", (unsigned)pet.stat(STAT_MAXHP));

    gfx_text(12,  188, 1, col::dim, "STR");
    gfx_text(44,  184, 2, col::white, "%u", (unsigned)pet.stat(STAT_STR));
    gfx_text(126, 188, 1, col::dim, "END");
    gfx_text(158, 184, 2, col::white, "%u", (unsigned)pet.stat(STAT_END));
    gfx_text(12,  212, 1, col::dim, "AGI");
    gfx_text(44,  208, 2, col::white, "%u", (unsigned)pet.stat(STAT_AGI));
    gfx_text(126, 212, 1, col::dim, "INT");
    gfx_text(158, 208, 2, col::white, "%u", (unsigned)pet.stat(STAT_INT));

    fb.drawFastHLine(12, 234, GAME_W - 24, col::dim);

    // --- bond: TIER only (exact value intentionally hidden) ---
    gfx_text(12, 240, 1, col::accent, "BOND");
    draw_heart(58, 250, 7, rgb565(255, 120, 160));
    gfx_text(74, 244, 2, col::white, "%s", pet.friendshipTier());

    // Raw drift axes, for checking the firmware against tools/personality_sim.py. Debug only:
    // showing these to a player would turn an emergent identity into a stat to min-max.
    if (app().debugOverlay) {
        const float* ax = app().drift.axes();
        gfx_text(12, 264, 1, col::good, "brv%+.2f eng%+.2f soc%+.2f wld%+.2f",
                 ax[AX_BRAVE], ax[AX_ENERGETIC], ax[AX_SOCIAL], ax[AX_WILD]);
    }

    // --- buttons ---
    RN_BTN.button("Rename", col::accent, col::black);
    BK_BTN.button("Back",   col::accent, col::black);
}

void SceneStats::onInput(const Input& in)
{
    if (!in.pressed) return;
    if (RN_BTN.contains(in)) { app().setScene(SceneId::Rename, Slide::Forward); return; }
    if (BK_BTN.contains(in)) { app().setScene(SceneId::Menu,   Slide::Back);    return; }
}
