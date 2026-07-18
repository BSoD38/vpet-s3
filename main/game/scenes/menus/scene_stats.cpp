#include "scene_stats.hpp"
#include "core/app.hpp"
#include "engine/gfx.hpp"
#include "assets/sprites.hpp"   // spr_unknown_data (fallback)
#include <cstdio>

static const Sprite SPR_FALLBACK { spr_unknown_data, SPRITE_W, SPRITE_H, SPRITE_TRANSP };  // "?" when a sprite can't be shown

// buttons
static const int RN_X = 16,  RN_Y = 274, RN_W = 100, RN_H = 34;   // Rename
static const int BK_X = 124, BK_Y = 274, BK_W = 100, BK_H = 34;   // Back

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

    fb.drawFastHLine(12, 72, GAME_W - 24, col::dim);

    // --- care meters ---
    gfx_text(12, 78, 1, col::accent, "CARE");
    const char* clab[3] = { "HUN", "HAP", "HEA" };
    float cval[3] = { p.hunger, p.happiness, p.health };
    for (int i = 0; i < 3; i++) {
        int y = 93 + i * 15;
        gfx_text(12, y + 1, 1, col::white, "%s", clab[i]);
        float f = cval[i] / 100.0f;
        gfx_bar(46, y, 148, 9, f, f < 0.30f ? col::warn : col::good, col::black, col::dim);
        gfx_text(200, y + 1, 1, col::dim, "%d", (int)cval[i]);
    }

    fb.drawFastHLine(12, 140, GAME_W - 24, col::dim);

    // --- battle stats (effective = creature base + trained modifier) ---
    gfx_text(12, 146, 1, col::accent, "BATTLE");
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

    // --- buttons ---
    fb.fillRoundRect(RN_X, RN_Y, RN_W, RN_H, 6, col::accent);
    gfx_text(RN_X + 14, RN_Y + 9, 2, col::black, "Rename");
    fb.fillRoundRect(BK_X, BK_Y, BK_W, BK_H, 6, col::accent);
    gfx_text(BK_X + 24, BK_Y + 9, 2, col::black, "Back");
}

void SceneStats::onInput(const Input& in)
{
    if (!in.pressed) return;
    if (hit(in.x, in.y, RN_X, RN_Y, RN_W, RN_H)) { app().setScene(SceneId::Rename, Slide::Forward); return; }
    if (hit(in.x, in.y, BK_X, BK_Y, BK_W, BK_H)) { app().setScene(SceneId::Menu,   Slide::Back);    return; }
}
