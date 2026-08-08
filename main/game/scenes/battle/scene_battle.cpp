#include "scene_battle.hpp"
#include "scene_battle_internal.hpp"
#include "core/app.hpp"
#include "engine/gfx.hpp"
#include "engine/util.hpp"      // clampf, randf
#include "engine/audio/sfx.hpp"
#include "sim/creatures.hpp"
#include "esp_random.h"         // battle seed
#include <cstring>
#include <cmath>

// Scene lifecycle, combat orchestration and effect spawning. All rendering lives in
// scene_battle_draw.cpp; shared constants in scene_battle_internal.hpp.

// The voice of one side of the fight (see VoiceProfile in engine/audio/audio.hpp). Battle is
// the only place in the game where two creatures are on screen at once, so it is the only
// place that has to say whose cry a sound is: left to the default, an enemy taking a hit
// would answer in the player's own voice, which reads as the PLAYER being hit.
//
// The returned profile points into the registry, which outlives every use of it here.
static audio::VoiceProfile voice_of(CreatureRegistry& reg, const Combatant& c)
{
    audio::VoiceProfile v;
    if (c.spriteIdx >= 0 && c.spriteIdx < reg.count()) {
        const Creature& cr = reg.at(c.spriteIdx);
        v.species = cr.id;
        v.family  = cr.voiceFamily;
        v.dir     = cr.dir;
        v.pitch   = cr.voicePitch;
    }
    return v;
}

// Pick an opponent from the registry nearest a target tier (excluding the egg and the
// player's own species); random among the nearest tier for variety.
static int pick_enemy_by_tier(CreatureRegistry& reg, int targetTier, int excludeIdx)
{
    int bestDiff = 999;
    for (int i = 0; i < reg.count(); i++) {
        if (i == excludeIdx || strcmp(reg.at(i).id, "egg") == 0) continue;
        int d = (int)reg.at(i).tier - targetTier; if (d < 0) d = -d;
        if (d < bestDiff) bestDiff = d;
    }
    int cand[CreatureRegistry::MAX]; int n = 0;
    for (int i = 0; i < reg.count(); i++) {
        if (i == excludeIdx || strcmp(reg.at(i).id, "egg") == 0) continue;
        int d = (int)reg.at(i).tier - targetTier; if (d < 0) d = -d;
        if (d == bestDiff && n < CreatureRegistry::MAX) cand[n++] = i;
    }
    if (n == 0) return excludeIdx >= 0 ? excludeIdx : 0;
    return cand[esp_random() % (uint32_t)n];
}

void SceneBattle::buildCombatants()
{
    Pet& pet = app().pet;
    CreatureRegistry& reg = app().creatures;

    Combatant p = combatant_from_pet(pet);

    int playerIdx  = pet.creatureIndex();
    int playerTier = pet.creature() ? pet.creature()->tier : 1;

    int   targetTier;
    float scale, aiSkill;
    bossFight_ = false;
    if (mode_ == BattleMode::Tower) {
        bossFight_ = (towerFloor_ % 5 == 0);
        targetTier = 1 + towerFloor_ / 4; if (targetTier > 4) targetTier = 4;  // climb tiers
        scale   = 0.85f + towerFloor_ * 0.12f;                                 // stronger each floor
        aiSkill = 0.40f + towerFloor_ * 0.03f; if (aiSkill > 0.95f) aiSkill = 0.95f;
        if (bossFight_) { scale *= 1.3f; aiSkill += 0.05f; if (aiSkill > 0.98f) aiSkill = 0.98f; }
    } else {  // Quick: a similar-tier rival, roughly even
        targetTier = playerTier;
        scale   = 0.90f + randf() * 0.25f;   // 0.90..1.15
        aiSkill = 0.50f + randf() * 0.15f;   // 0.50..0.65
    }

    int enemyIdx = pick_enemy_by_tier(reg, targetTier, playerIdx);
    Combatant e = combatant_from_creature(reg.at(enemyIdx), enemyIdx, scale, aiSkill);

    battle_.begin(p, e, esp_random());
    battle_.setAuto(0, false);
    battle_.setAuto(1, true);

    // Get the opponent's own sounds loading now, during the intro, rather than letting the
    // first hit trigger the load and answer in the shared voice while it arrives.
    {
        const Creature& ec = reg.at(enemyIdx);
        audio::request_voice(ec.id, ec.dir);
    }

    for (int i = 0; i < 2; i++) {
        float hp = (float)battle_.side(i).hp;
        hpFront_[i] = hpGhost_[i] = hpPrev_[i] = hp;
        hpHold_[i] = 0.0f;
    }
}

void SceneBattle::onEnter()
{
    t_ = 0.0f;
    armed_ = Move::Strike;
    ring_ = Ring::None; ringT_ = 0.0f;
    comboStep_ = -1; comboSum_ = 0.0f;
    judgeText_ = nullptr; judgeT_ = 0.0f;
    tapped_ = false;
    done_ = false; resultT_ = 0.0f;
    shake_ = shakeX_ = shakeY_ = 0.0f;
    shock_ = 0.0f;
    for (int i = 0; i < 2; i++) { lunge_[i] = flash_[i] = faint_[i] = 0.0f; }
    for (auto& p : pops_)  p.used = false;
    for (auto& p : parts_) p.used = false;
    buildCombatants();
}

void SceneBattle::onExit() {}

void SceneBattle::startRing(Ring k)
{
    if (k == Ring::Attack && armed_ == Move::Special && !battle_.canSpecial(0)) armed_ = Move::Strike;
    ring_ = k;
    ringT_ = 0.0f;
    comboStep_ = (k == Ring::Attack && armed_ == Move::Special) ? 0 : -1;
    comboSum_ = 0.0f;
}

float SceneBattle::ringRadius() const { return RING_RMAX + (RING_RMIN - RING_RMAX) * ringT_; }

float SceneBattle::ringQuality() const
{
    return clampf(1.0f - fabsf(ringRadius() - RING_RTARGET) / RING_WINDOW, 0.0f, 1.0f);
}

void SceneBattle::setJudge(const char* text, uint16_t color)
{
    judgeText_ = text; judgeColor_ = color; judgeT_ = JUDGE_DUR;
}

void SceneBattle::resolveRing(float q)
{
    uint16_t cyan = rgb565(120, 200, 255);
    if (ring_ == Ring::Attack) {
        Move used = armed_;
        if (used == Move::Special) {
            if (q >= 0.75f)      setJudge("SPECIAL!!", cyan);
            else if (q >= 0.45f) setJudge("SPECIAL!",  cyan);
            else                 setJudge("fizzle...", col::dim);
        } else {
            if (q >= 0.85f)      setJudge("PERFECT!!", col::accent);
            else if (q >= 0.60f) setJudge("GREAT!",    col::accent);
            else if (q >= 0.35f) setJudge("GOOD",      col::white);
            else                 setJudge("MISS",      col::warn);
        }
        battle_.submitAttack(used, q);
        if (used == Move::Special) armed_ = Move::Strike;   // meter's spent -> pre-select Strike right away
    } else if (ring_ == Ring::Parry) {
        if (q >= 0.85f)      setJudge("PERFECT PARRY!", cyan);
        else if (q >= 0.60f) setJudge("GREAT PARRY!",   cyan);
        else if (q >= 0.35f) setJudge("PARRY",          col::white);
        else                 setJudge("MISS",           col::warn);
        if (q >= 0.60f) {   // reward a good parry with extra juice
            spawnShock(0, cyan);
            spawnParticles(0, 12, cyan);
            if (shake_ < 0.5f) shake_ = 0.5f;
            shakeAmp_ = SHAKE_AMP;
        }
        battle_.submitParry(q);
    }
    ring_ = Ring::None; ringT_ = 0.0f; comboStep_ = -1; comboSum_ = 0.0f;
}

void SceneBattle::spawnPopup(int side, int32_t amount, uint8_t kind)
{
    int cx = (side == 0 ? PLAYER_CX : ENEMY_CX);
    int cy = (side == 0 ? PLAYER_CY : ENEMY_CY) - 34;
    for (auto& p : pops_) if (!p.used) {
        p.used = true; p.x = (float)(cx - 12); p.y = (float)cy;
        p.vy = -30.0f; p.life = 0.9f; p.amount = amount; p.kind = kind;
        return;
    }
}

void SceneBattle::spawnParticles(int side, int n, uint16_t color)
{
    int cx = (side == 0 ? PLAYER_CX : ENEMY_CX);
    int cy = (side == 0 ? PLAYER_CY : ENEMY_CY);
    for (int k = 0; k < n; k++) {
        for (auto& p : parts_) if (!p.used) {
            p.used = true; p.x = (float)cx; p.y = (float)cy;
            p.vx = (randf() - 0.5f) * 150.0f;
            p.vy = -20.0f - randf() * 130.0f;
            p.life = 0.5f + randf() * 0.3f;
            p.color = color;
            break;
        }
    }
}

void SceneBattle::spawnShock(int side, uint16_t color)
{
    shockX_ = (side == 0 ? PLAYER_CX : ENEMY_CX);
    shockY_ = (side == 0 ? PLAYER_CY : ENEMY_CY);
    shock_ = SHOCK_DUR; shockColor_ = color;
}

void SceneBattle::drainEvents()
{
    uint16_t cyan = rgb565(120, 200, 255);
    BattleEventRec ev;
    while (battle_.pollEvent(ev)) {
        switch (ev.type) {
            case BattleEvent::AttackStart:
                lunge_[ev.actor] = LUNGE_DUR;
                if (ev.move == Move::Special && ev.actor == 1) setJudge("ENEMY SPECIAL!", col::warn);
                break;
            case BattleEvent::Hit:
            case BattleEvent::Crit:
            case BattleEvent::Glance: {
                bool sp = (ev.move == Move::Special);
                uint8_t kind = sp ? 3 : (ev.type == BattleEvent::Crit ? 1 : ev.type == BattleEvent::Glance ? 2 : 0);
                // Impact weight follows the visuals: a special or a crit gets the bigger
                // sound, a glancing blow the same sound played back softer. Voiced as the
                // creature being HIT, not the one swinging -- an impact sound is a reaction,
                // and it is the target the popup, the flash and the shake all belong to too.
                audio::Params ip;
                audio::VoiceProfile tv = voice_of(app().creatures, battle_.side(ev.target));
                ip.voice = &tv;
                if (sp || ev.type == BattleEvent::Crit) audio::play(sfx::kCrit, ip);
                else {
                    ip.gain = (ev.type == BattleEvent::Glance) ? 0.45f : 1.0f;
                    audio::play(sfx::kHit, ip);
                }
                spawnPopup(ev.target, ev.amount, kind);
                flash_[ev.target] = (ev.type == BattleEvent::Glance) ? FLASH_DUR * 0.6f : FLASH_DUR;
                if (sp) {
                    spawnParticles(ev.target, 28, cyan);
                    spawnShock(ev.target, cyan);
                    shake_ = 1.0f; shakeAmp_ = SHAKE_AMP_BIG;   // specials shake the whole UI, hard
                } else if (ev.type == BattleEvent::Crit) {
                    spawnParticles(ev.target, 14, col::accent);
                    shake_ = 1.0f; shakeAmp_ = SHAKE_AMP;
                }
                break;
            }
            // A parry outcome is emitted for the DEFENDER of every attack, including the
            // enemy's when the player is the one attacking. These two sounds are feedback on
            // the PLAYER'S timing, so they only make sense for the player's own parries --
            // unfiltered, a landed hit came with the enemy's "miss" whoosh over the top of it,
            // and against a high-skill enemy the player heard their own success sting for
            // every attack they made.
            case BattleEvent::ParryOk:
                if (ev.actor == 0) sfx::play(sfx::kParry);
                break;
            case BattleEvent::ParryMiss:
                if (ev.actor == 0) sfx::play(sfx::kMiss);
                break;
            case BattleEvent::Dodge: {   // fully evaded (perfect parry + AGI)
                uint16_t ec = rgb565(150, 230, 255);
                // ev.actor is whoever dodged, and the same event means opposite things to the
                // player: their own dodge is the success sting, the enemy slipping their
                // attack is a whiff. (The visuals already read from ev.actor; only the sound
                // was saying "well done" for both.)
                sfx::play(ev.actor == 0 ? sfx::kParry : sfx::kMiss);
                setJudge("DODGE!", ec);
                spawnShock(ev.actor, ec);
                spawnParticles(ev.actor, 10, ec);
                break;
            }
            case BattleEvent::Faint:
                faint_[ev.actor] = FAINT_DUR; shake_ = 1.0f; shakeAmp_ = SHAKE_AMP;
                spawnParticles(ev.actor, 24, col::warn);
                break;
            default: break;
        }
    }
}

void SceneBattle::onInput(const Input& in)
{
    if (!in.pressed) return;
    tapped_ = true; tapX_ = in.x; tapY_ = in.y;
}

void SceneBattle::update(float dt)
{
    t_ += dt;

    for (int i = 0; i < 2; i++) {
        float real = (float)battle_.side(i).hp;
        if (real < hpPrev_[i] - 0.5f) hpHold_[i] = GHOST_DELAY;          // took a hit -> refresh phantom hold
        hpPrev_[i] = real;
        hpFront_[i] += (real - hpFront_[i]) * clampf(HP_FRONT_EASE * dt, 0.0f, 1.0f);
        if (real >= hpGhost_[i]) { hpGhost_[i] = real; hpHold_[i] = 0.0f; }   // healed/equal -> snap phantom up
        else if (hpHold_[i] > 0.0f) hpHold_[i] -= dt;                    // holding
        else hpGhost_[i] += (hpFront_[i] - hpGhost_[i]) * clampf(GHOST_EASE * dt, 0.0f, 1.0f);
    }
    for (int i = 0; i < 2; i++) {
        lunge_[i] = lunge_[i] > dt ? lunge_[i] - dt : 0.0f;
        flash_[i] = flash_[i] > dt ? flash_[i] - dt : 0.0f;
        faint_[i] = faint_[i] > dt ? faint_[i] - dt : 0.0f;
    }
    if (judgeT_ > 0.0f) judgeT_ -= dt;
    if (shock_ > 0.0f)  shock_ = shock_ > dt ? shock_ - dt : 0.0f;
    if (shake_ > 0.0f) {
        shake_ = shake_ > SHAKE_DECAY * dt ? shake_ - SHAKE_DECAY * dt : 0.0f;
        shakeX_ = (randf() - 0.5f) * 2.0f * shakeAmp_ * shake_;
        shakeY_ = (randf() - 0.5f) * 2.0f * shakeAmp_ * shake_;
    } else { shakeX_ = shakeY_ = 0.0f; }
    for (auto& p : pops_) if (p.used) {
        p.y += p.vy * dt; p.vy += 46.0f * dt; p.life -= dt;
        if (p.life <= 0.0f) p.used = false;
    }
    for (auto& p : parts_) if (p.used) {
        p.x += p.vx * dt; p.y += p.vy * dt; p.vy += 200.0f * dt; p.life -= dt;
        if (p.life <= 0.0f) p.used = false;
    }

    if (done_) {
        resultT_ += dt;
        if (tapped_ && resultT_ > 0.4f) app().setScene(SceneId::BattleSelect, Slide::Iris);
        tapped_ = false;
        return;
    }

    if (ring_ != Ring::None) {
        float dur = (ring_ == Ring::Attack) ? (comboStep_ >= 0 ? RING_DUR_COMBO : RING_DUR_ATK) : RING_DUR_PARRY;
        ringT_ += dt / dur;
        bool locked = tapped_, timeout = ringT_ >= 1.0f;
        if (locked || timeout) {
            float q = locked ? ringQuality() : 0.12f;
            if (comboStep_ >= 0) {                          // Special: accumulate a 3-hit combo
                comboSum_ += q;
                spawnParticles(1, 4, rgb565(120, 200, 255));
                comboStep_++;
                if (comboStep_ >= COMBO_HITS) resolveRing(comboSum_ / (float)COMBO_HITS);
                else                          ringT_ = 0.0f;
            } else {
                resolveRing(q);
            }
        }
    } else {
        if (tapped_) {
            if (hit(tapX_, tapY_, STRIKE_BX, BTN_Y, BTN_W, BTN_H))
                armed_ = Move::Strike;
            else if (hit(tapX_, tapY_, SPECIAL_BX, BTN_Y, BTN_W, BTN_H) && battle_.canSpecial(0))
                armed_ = Move::Special;
        }
        battle_.update(dt);
        if (battle_.awaitingAttack())      startRing(Ring::Attack);
        else if (battle_.awaitingParry())  startRing(Ring::Parry);
    }

    drainEvents();
    if (battle_.finished() && !done_) {
        done_ = true; resultT_ = 0.0f;
        const Combatant& p = battle_.side(0);
        float hpFrac = p.maxHp ? (float)p.hp / (float)p.maxHp : 0.0f;
        bool won = battle_.winner() == 0;
        // Fanfare over the top of the battle track rather than after it: the result screen
        // is the payoff, and waiting out a fade would put the sting in the wrong place.
        sfx::play(won ? sfx::kWin : sfx::kLose);
        outcome_ = app().pet.applyBattleResult(won, hpFrac);
        if (mode_ == BattleMode::Tower) {   // advance on win, drop to last checkpoint on loss
            int next = won ? towerFloor_ + 1 : ((towerFloor_ - 1) / 5) * 5 + 1;
            if (next < 1) next = 1;
            if (next > 255) next = 255;
            app().save.storeU8("twr", (uint8_t)next);
        }
    }
    tapped_ = false;
}
