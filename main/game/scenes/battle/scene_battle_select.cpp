#include "scene_battle_select.hpp"
#include "core/app.hpp"
#include "engine/gfx.hpp"
#include "scenes/battle/scene_battle.hpp"   // BattleMode

// mode cards + back button
static const int CARD_X = 20, CARD_W = 200, CARD_H = 66;
static const int QK_Y = 84, TW_Y = 162;
static const int BK_X = 70, BK_Y = 272, BK_W = 100, BK_H = 34;

static const int MIN_BATTLE_HP = 20;   // below this HP% the pet is too weak to fight (must heal)

void SceneBattleSelect::render()
{
    fb.fillScreen(col::panel);
    gfx_text(20, 22, 3, col::accent, "BATTLE");

    int floor = app().save.loadU8("twr", 1);
    bool boss = (floor % 5 == 0);

    // Quick Battle
    fb.fillRoundRect(CARD_X, QK_Y, CARD_W, CARD_H, 10, rgb565(52, 58, 84));
    fb.drawRoundRect(CARD_X, QK_Y, CARD_W, CARD_H, 10, col::accent);
    gfx_text(CARD_X + 16, QK_Y + 14, 2, col::white, "QUICK BATTLE");
    gfx_text(CARD_X + 16, QK_Y + 42, 1, col::dim, "vs a similar-tier rival");

    // Tower
    fb.fillRoundRect(CARD_X, TW_Y, CARD_W, CARD_H, 10, rgb565(52, 58, 84));
    fb.drawRoundRect(CARD_X, TW_Y, CARD_W, CARD_H, 10, boss ? col::warn : col::accent);
    gfx_text(CARD_X + 16, TW_Y + 12, 2, col::white, "TOWER");
    gfx_text(CARD_X + 16, TW_Y + 40, 1, boss ? col::warn : col::dim,
             "Floor %d%s", floor, boss ? "   - BOSS!" : "");

    // readiness hint (below MIN_BATTLE_HP the cards are disabled until the pet heals)
    int hp = (int)app().pet.state().health;
    bool tooWeak = hp < MIN_BATTLE_HP;
    gfx_text(20, 240, 1, tooWeak ? col::warn : col::dim,
             "Your HP: %d%%%s", hp, tooWeak ? "   too weak - heal first!" : "");

    // Back
    fb.fillRoundRect(BK_X, BK_Y, BK_W, BK_H, 6, col::accent);
    gfx_text(BK_X + 24, BK_Y + 9, 2, col::black, "Back");
}

void SceneBattleSelect::onInput(const Input& in)
{
    if (!in.pressed) return;
    bool tooWeak = (int)app().pet.state().health < MIN_BATTLE_HP;
    if (hit(in.x, in.y, CARD_X, QK_Y, CARD_W, CARD_H)) {
        if (tooWeak) return;                 // gated: heal before fighting
        app().battle.setup(BattleMode::Quick, 0);
        app().setScene(SceneId::Battle, Slide::Iris);
        return;
    }
    if (hit(in.x, in.y, CARD_X, TW_Y, CARD_W, CARD_H)) {
        if (tooWeak) return;                 // gated: heal before fighting
        int floor = app().save.loadU8("twr", 1);
        app().battle.setup(BattleMode::Tower, floor);
        app().setScene(SceneId::Battle, Slide::Iris);
        return;
    }
    if (hit(in.x, in.y, BK_X, BK_Y, BK_W, BK_H)) {
        app().setScene(SceneId::Menu, Slide::Back);
        return;
    }
}
