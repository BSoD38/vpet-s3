#include "scene_battle.hpp"
#include "scene_battle_internal.hpp"
#include "core/app.hpp"
#include "engine/gfx.hpp"
#include "engine/util.hpp"      // clampf
#include "sim/creatures.hpp"    // attr_color / attr_short
#include <cstring>
#include <cstdio>
#include <cmath>

// All rendering for the battle scene. Logic/orchestration lives in scene_battle.cpp;
// shared constants in scene_battle_internal.hpp.

void SceneBattle::drawArena()
{
    uint16_t top = rgb565(26, 20, 44), mid = rgb565(58, 38, 82), bot = rgb565(28, 22, 46);
    for (int y = 0; y < GAME_H; y += 4) {
        float f = (float)y / GAME_H;
        uint16_t c = f < 0.5f ? mix565(top, mid, f * 2.0f) : mix565(mid, bot, (f - 0.5f) * 2.0f);
        fb.fillRect(0, y, GAME_W, 4, c);
    }
    static const int SX[] = {30, 200, 70, 170, 110, 220, 50};
    static const int SY[] = {60, 48, 130, 150, 70, 120, 168};
    for (int i = 0; i < 7; i++) fb.fillCircle(SX[i], SY[i], 1, rgb565(120, 120, 160));
    fb.fillRoundRect(ENEMY_CX - 40 + (int)shakeX_,  ENEMY_CY + 30,  80, 12, 6, rgb565(18, 14, 30));
    fb.fillRoundRect(PLAYER_CX - 40 + (int)shakeX_, PLAYER_CY + 30, 80, 12, 6, rgb565(18, 14, 30));
}

void SceneBattle::drawCombatant(int side)
{
    const Combatant& c = battle_.side(side);
    int baseX = (side == 0 ? PLAYER_CX : ENEMY_CX);
    int baseY = (side == 0 ? PLAYER_CY : ENEMY_CY);
    int dir   = (side == 0 ? -1 : +1);

    float loff = 0.0f;
    if (lunge_[side] > 0.0f) { float p = 1.0f - lunge_[side] / LUNGE_DUR; loff = LUNGE_AMP * sinf(p * 3.14159f); }
    float fi = flash_[side] > 0.0f ? flash_[side] / FLASH_DUR : 0.0f;
    float kb = -dir * KB_AMP * fi;
    float bob = sinf(t_ * BOB_SPD + side * 3.14159f) * BOB_AMP;
    float sink = faint_[side] > 0.0f ? (1.0f - faint_[side] / FAINT_DUR) * 18.0f
                                     : (c.hp <= 0 ? 18.0f : 0.0f);

    int cx = baseX + (int)shakeX_;
    int cy = baseY + (int)(dir * loff + kb + bob + sink) + (int)shakeY_;

    LGFX_Sprite* spr = app().creatures.sprite(c.spriteIdx);
    if (spr) gfx_blit_sprite_fit(spr, cx, cy, SPRITE_BOX, SPRITE_BOX, SPRITE_TRANSP);
    else { fb.fillRoundRect(cx - 24, cy - 24, 48, 48, 8, col::dim); gfx_text(cx - 6, cy - 8, 2, col::black, "?"); }

    if (fi > 0.0f) {
        int r = (int)(8 + (1.0f - fi) * 22.0f);
        fb.drawCircle(cx, cy, r,     col::white);
        fb.drawCircle(cx, cy, r + 1, mix565(col::white, col::accent, 0.5f));
    }
}

void SceneBattle::drawGauge(int x, int y, int w, int h, float frac, uint16_t lo, uint16_t hi, uint16_t border, bool glow)
{
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    int rad = h / 2; if (rad < 2) rad = 2;
    fb.fillRoundRect(x, y, w, h, rad, rgb565(16, 16, 24));           // recessed track
    int fw = (int)(w * frac);
    if (fw >= 3) {
        int fr = fw < h ? fw / 2 : rad;
        fb.fillRoundRect(x, y, fw, h, fr, lo);                      // fill
        int gh = h / 2; if (gh < 1) gh = 1;
        int gw = fw - 2;
        if (gw > 0) fb.fillRoundRect(x + 1, y + 1, gw, gh, gh > 1 ? gh / 2 : 1, hi);   // top gloss band
    }
    fb.drawRoundRect(x, y, w, h, rad, border);                       // rim
    if (glow) fb.drawRoundRect(x - 1, y - 1, w + 2, h + 2, rad + 1, mix565(border, col::white, 0.6f));
}

// HP gauge with a lagging dark-red "phantom" drawn behind the live fill.
void SceneBattle::drawHpGauge(int x, int y, int w, int h, float frac, float ghost, uint16_t lo, uint16_t hi, uint16_t border)
{
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    if (ghost < frac) ghost = frac;
    if (ghost > 1) ghost = 1;
    int rad = h / 2; if (rad < 2) rad = 2;
    fb.fillRoundRect(x, y, w, h, rad, rgb565(16, 16, 24));           // recessed track
    int gw = (int)(w * ghost);                                       // phantom (recently-lost HP)
    if (gw >= 3) { int gr = gw < h ? gw / 2 : rad; fb.fillRoundRect(x, y, gw, h, gr, rgb565(120, 24, 24)); }
    int fw = (int)(w * frac);                                        // live fill
    if (fw >= 3) {
        int fr = fw < h ? fw / 2 : rad;
        fb.fillRoundRect(x, y, fw, h, fr, lo);
        int gh = h / 2; if (gh < 1) gh = 1;
        int gwid = fw - 2;
        if (gwid > 0) fb.fillRoundRect(x + 1, y + 1, gwid, gh, gh > 1 ? gh / 2 : 1, hi);   // gloss
    }
    fb.drawRoundRect(x, y, w, h, rad, border);
}

void SceneBattle::drawHud()
{
    const Combatant& e = battle_.side(1);
    const Combatant& p = battle_.side(0);
    int sx = (int)shakeX_, sy = (int)shakeY_;

    // enemy: "Name - (ATTR)", HP (with phantom), ATB
    gfx_text(10 + sx, 6 + sy, 2, col::white, "%s -", e.name);
    gfx_text(10 + ((int)strlen(e.name) + 2) * 12 + sx, 6 + sy, 2, attr_color(e.attribute), " (%s)", attr_short(e.attribute));
    if (bossFight_) gfx_text(GAME_W - 4 * 12 - 6 + sx, 6 + sy, 2, col::warn, "BOSS");
    drawHpGauge(10 + sx, 26 + sy, 150, 11, e.maxHp ? hpFront_[1] / e.maxHp : 0.0f, e.maxHp ? hpGhost_[1] / e.maxHp : 0.0f,
                rgb565(210, 60, 60), rgb565(255, 150, 140), col::white);
    drawGauge(10 + sx, 40 + sy, 110, 6, e.atb, rgb565(230, 180, 70), rgb565(255, 230, 150), rgb565(90, 80, 50), false);

    // player: "Name - (ATTR)", HP (with phantom) + numeric HP/MaxHP, ATB, special
    gfx_text(10 + sx, 224 + sy, 2, col::white, "%s -", p.name);
    gfx_text(10 + ((int)strlen(p.name) + 2) * 12 + sx, 224 + sy, 2, attr_color(p.attribute), " (%s)", attr_short(p.attribute));
    drawHpGauge(10 + sx, 244 + sy, 220, 11, p.maxHp ? hpFront_[0] / p.maxHp : 0.0f, p.maxHp ? hpGhost_[0] / p.maxHp : 0.0f,
                rgb565(50, 190, 90), rgb565(150, 245, 170), col::white);
    char hpbuf[24];
    snprintf(hpbuf, sizeof hpbuf, "%d/%d", (int)battle_.side(0).hp, (int)p.maxHp);
    int hx = 10 + 220 - (int)strlen(hpbuf) * 6 - 5, hy = 246;
    gfx_text(hx + 1 + sx, hy + 1 + sy, 1, rgb565(8, 8, 12), "%s", hpbuf);   // shadow for legibility over the bar
    gfx_text(hx + sx,     hy + sy,     1, col::white,        "%s", hpbuf);

    drawGauge(10 + sx, 258 + sy, 220, 7, p.atb, rgb565(230, 180, 70), rgb565(255, 230, 150), rgb565(90, 80, 50), false);

    bool full = p.meter >= 1.0f;
    uint16_t slo = full ? mix565(rgb565(80, 170, 255), col::white, 0.5f + 0.5f * sinf(t_ * 10.0f))
                        : rgb565(70, 130, 220);
    drawGauge(10 + sx, 268 + sy, 220, 7, p.meter, slo, rgb565(160, 210, 255),
              full ? col::white : rgb565(50, 70, 110), full);
}

void SceneBattle::drawBtn(int x, const char* label, bool armed, bool enabled, float pulse)
{
    int bx = x + (int)shakeX_, by = BTN_Y + (int)shakeY_;
    uint16_t base = !enabled ? rgb565(40, 42, 52) : (armed ? col::accent : col::card);
    fb.fillRoundRect(bx, by, BTN_W, BTN_H, 7, base);
    int gh = (int)((BTN_H - 4) * 0.42f);
    fb.fillRoundRect(bx + 3, by + 2, BTN_W - 6, gh, 4, mix565(base, col::white, 0.32f));   // gloss
    fb.drawFastHLine(bx + 5, by + 2, BTN_W - 10, mix565(base, col::white, 0.6f));          // highlight
    uint16_t rim = (armed && enabled) ? mix565(col::accent, col::white, pulse) : rgb565(18, 18, 26);
    fb.drawRoundRect(bx, by, BTN_W, BTN_H, 7, rim);
    if (armed && enabled) fb.drawRoundRect(bx - 1, by - 1, BTN_W + 2, BTN_H + 2, 8, rim);
    uint16_t txt = !enabled ? col::dim : (armed ? col::black : col::white);
    int tw = (int)strlen(label) * 12;
    gfx_text(bx + (BTN_W - tw) / 2, by + (BTN_H - 16) / 2, 2, txt, "%s", label);
}

void SceneBattle::drawButtons()
{
    bool spOk = battle_.canSpecial(0);
    float atb = battle_.side(0).atb;
    float pulse = atb > 0.7f ? (0.5f + 0.5f * sinf(t_ * 12.0f)) : 0.0f;
    drawBtn(STRIKE_BX,  "STRIKE",  armed_ == Move::Strike,  true, pulse);
    drawBtn(SPECIAL_BX, "SPECIAL", armed_ == Move::Special, spOk, spOk ? pulse : 0.0f);
}

void SceneBattle::drawRing()
{
    if (ring_ == Ring::None) return;
    int cx = (ring_ == Ring::Attack ? ENEMY_CX : PLAYER_CX) + (int)shakeX_;
    int cy = (ring_ == Ring::Attack ? ENEMY_CY : PLAYER_CY) + (int)shakeY_;
    bool special = (comboStep_ >= 0);
    uint16_t c = special ? rgb565(120, 200, 255)
                         : (ring_ == Ring::Attack ? col::warn : rgb565(90, 180, 240));

    // Rings are drawn with a dark halo (inner + outer) so the bright core stays legible
    // even overlaid on a big, busy creature sprite.
    uint16_t halo = rgb565(6, 6, 10);
    int RT = (int)RING_RTARGET;
    fb.drawCircle(cx, cy, RT - 1, halo);          // fixed target ring
    fb.drawCircle(cx, cy, RT,     col::white);
    fb.drawCircle(cx, cy, RT + 1, col::white);
    fb.drawCircle(cx, cy, RT + 2, halo);
    int r = (int)ringRadius();                    // shrinking ring
    float near = clampf(1.0f - fabsf((float)r - RING_RTARGET) / RING_WINDOW, 0.0f, 1.0f);
    uint16_t rc = mix565(c, col::white, near);
    fb.drawCircle(cx, cy, r - 1, halo);
    fb.drawCircle(cx, cy, r,     rc);
    fb.drawCircle(cx, cy, r + 1, rc);
    fb.drawCircle(cx, cy, r + 2, halo);

    const char* h = special ? "SPECIAL!" : (ring_ == Ring::Attack ? "TAP!" : "PARRY!");
    int tw = (int)strlen(h) * 12;
    gfx_text(cx - tw / 2, cy - (int)RING_RMAX - 18, 2, c, "%s", h);

    if (special) {   // combo progress pips
        int px = cx - (COMBO_HITS * 10) / 2;
        for (int i = 0; i < COMBO_HITS; i++) {
            uint16_t pc = i < comboStep_ ? c : rgb565(60, 70, 90);
            fb.fillCircle(px + i * 10 + 3, cy + (int)RING_RMAX + 14, 3, pc);
        }
    }
}

void SceneBattle::drawEffects()
{
    int sx = (int)shakeX_, sy = (int)shakeY_;

    if (shock_ > 0.0f) {
        float p = 1.0f - shock_ / SHOCK_DUR;
        int r = (int)(6 + p * 40.0f);
        uint16_t c = mix565(shockColor_, rgb565(24, 20, 40), p);
        fb.drawCircle(shockX_ + sx, shockY_ + sy, r,     c);
        fb.drawCircle(shockX_ + sx, shockY_ + sy, r + 1, c);
        if (r > 2) fb.drawCircle(shockX_ + sx, shockY_ + sy, r - 1, mix565(c, col::white, 0.4f));
    }

    for (auto& pt : parts_) if (pt.used) {
        float f = clampf(pt.life / 0.28f, 0.0f, 1.0f);            // fade over the last ~0.28s
        uint16_t c = mix565(rgb565(40, 30, 60), pt.color, f);      // dim toward the arena as it dies
        fb.fillCircle((int)pt.x + sx, (int)pt.y + sy, f < 0.4f ? 1 : 2, c);
    }

    for (auto& pp : pops_) if (pp.used) {
        uint16_t c; uint8_t sz; char buf[16];
        switch (pp.kind) {
            case 1: c = col::accent;          sz = 3; snprintf(buf, sizeof buf, "%d!",  (int)pp.amount); break;
            case 2: c = col::dim;             sz = 2; snprintf(buf, sizeof buf, "%d",   (int)pp.amount); break;
            case 3: c = rgb565(150, 215, 255);sz = 3; snprintf(buf, sizeof buf, "%d!!", (int)pp.amount); break;
            default:c = col::white;           sz = 2; snprintf(buf, sizeof buf, "%d",   (int)pp.amount); break;
        }
        if (pp.life < 0.3f) c = mix565(rgb565(30, 24, 46), c, pp.life / 0.3f);
        gfx_text((int)pp.x + sx, (int)pp.y + sy, sz, c, "%s", buf);
    }
}

void SceneBattle::drawJudge()
{
    if (judgeT_ <= 0.0f || !judgeText_) return;
    float p = judgeT_ / JUDGE_DUR;
    uint16_t c = judgeColor_;
    if (p < 0.4f) c = mix565(rgb565(30, 24, 46), judgeColor_, p / 0.4f);
    int len = (int)strlen(judgeText_);
    uint8_t sz = (len * 18 > GAME_W - 8) ? 2 : 3;   // drop long banners to size-2 so they fit on one line
    int tw = len * (sz == 3 ? 18 : 12);
    int yy = 148 - (int)((1.0f - p) * 12.0f);
    gfx_text((GAME_W - tw) / 2, yy, sz, c, "%s", judgeText_);
}

void SceneBattle::drawResult()
{
    if (!done_) return;
    int bw = 212, bh = 132, bx = (GAME_W - bw) / 2, by = (GAME_H - bh) / 2;
    fb.fillRoundRect(bx, by, bw, bh, 12, rgb565(18, 14, 30));
    bool win = outcome_.won;
    uint16_t c = win ? col::good : col::warn;
    fb.drawRoundRect(bx, by, bw, bh, 12, c);
    fb.drawRoundRect(bx + 1, by + 1, bw - 2, bh - 2, 11, c);

    const char* t = win ? "VICTORY" : "DEFEAT";
    gfx_text((GAME_W - (int)strlen(t) * 18) / 2, by + 14, 3, c, "%s", t);

    // outcome detail (size-1 lines, centered)
    char line[40];
    if (win) snprintf(line, sizeof line, "+%u stats   +%d bond", (unsigned)outcome_.statGain, outcome_.friendDelta);
    else     snprintf(line, sizeof line, "%d bond   HP critical", outcome_.friendDelta);
    gfx_text((GAME_W - (int)strlen(line) * 6) / 2, by + 48, 1, col::white, "%s", line);

    char hl[24];
    snprintf(hl, sizeof hl, "HP now %d%%", outcome_.healthPct);
    gfx_text((GAME_W - (int)strlen(hl) * 6) / 2, by + 62, 1, col::dim, "%s", hl);

    if (mode_ == BattleMode::Tower) {
        char tl[28];
        if (win) snprintf(tl, sizeof tl, "Floor %d cleared!", towerFloor_);
        else     snprintf(tl, sizeof tl, "fell to floor %d", ((towerFloor_ - 1) / 5) * 5 + 1);
        gfx_text((GAME_W - (int)strlen(tl) * 6) / 2, by + 78, 1, win ? col::good : col::warn, "%s", tl);
    }

    if (outcome_.gotSick) {
        const char* s = "caught a cold!";
        gfx_text((GAME_W - (int)strlen(s) * 6) / 2, by + 92, 1, col::warn, "%s", s);
    }

    if (resultT_ > 0.5f) {
        const char* s = "tap to exit";
        gfx_text((GAME_W - (int)strlen(s) * 12) / 2, by + 110, 2, col::dim, "%s", s);
    }
}

void SceneBattle::render()
{
    drawArena();
    drawCombatant(1);
    drawCombatant(0);
    drawEffects();
    drawRing();
    drawJudge();
    drawHud();
    drawButtons();
    drawResult();
}
