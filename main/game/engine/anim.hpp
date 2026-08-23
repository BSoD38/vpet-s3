#pragma once
#include <cstdint>
#include <cmath>
#include "sim/creatures.hpp"   // SheetFrame indices

// Sprite-frame state machine for a creature on screen.
//
// Two layers: a persistent BASE the scene derives from sim state every frame
// (Idle / Nap / Sick), and a one-shot REACTION (Eat / Nope / Happy / ...) that
// plays for a duration then falls back to base. frame() returns a SheetFrame
// index for CreatureRegistry::frame(); single-pose creatures show their one
// sprite for any index, so this runs unconditionally with no special-casing.
//
// Movement stays with the scenes (bob/hop/wiggle offsets already live there) and this class
// only decides WHICH pose is showing -- but it does expose the walk-cycle phase, because the
// Idle pair is a two-frame WALK and ground locomotion has to advance in lockstep with it.
// See engine/walk.hpp, which derives travel from stepPhase() rather than from a clock of
// its own.

// One footfall per Idle frame. This is therefore not just a cosmetic cadence: it sets how
// fast a walking creature covers ground (WALK_STRIDE px per frame, engine/walk.hpp), so the
// two constants are a tuning PAIR -- shortening this speeds the creature up. The DEFAULT:
// setStepSecs() stretches it per instance (an Elderly/Twilight creature walks slower --
// docs/death-and-lifespan.md §4), and because travel derives from the phase, the gait and
// the ground speed slow together with no second clock to fight.
constexpr float ANIM_STEP_SECS = 0.45f;

enum class Anim : uint8_t {
    Idle,     // 0/1 alternating
    Happy,    // 2
    Angry,    // 3
    Train,    // 4/5 alternating
    Attack,   // 6/7 alternating
    Eat,      // 8/9 alternating
    Nope,     // 10 (refusal head-shake)
    Nap,      // 12/13 alternating (slow)
    Sick,     // 14
    Lose,     // 15
};

class CreatureAnim {
public:
    void setBase(Anim a)             { base_ = a; }
    void setStepSecs(float s)        { stepSecs_ = s > 0.05f ? s : 0.05f; }   // footfall period
    void react(Anim a, float secs)   { overlay_ = a; overlayLeft_ = secs; }
    void face(bool mirrored)         { facing_ = mirrored; }   // movement facing (scene decides)
    void tick(float dt)              { t_ += dt; if (overlayLeft_ > 0.0f) overlayLeft_ -= dt; }
    bool reacting() const            { return overlayLeft_ > 0.0f; }

    // True while the two-frame walk cycle is the pose actually on screen. Locomotion keys
    // off this, which is why a napping / sick / eating / refusing creature stands still
    // without movement code needing a condition for any of those states.
    bool walkCycle() const           { return !reacting() && base_ == Anim::Idle; }

    // Progress through the current footfall, 0..1, hitting exactly 0 at every frame change
    // (same divisor as flip()). Travel derived from this cannot skate: change the frame
    // rate, the step cadence or dt and the feet still land where the ground says they do.
    float stepPhase() const          { return fmodf(t_, stepSecs_) / stepSecs_; }

    // Left-to-right flip for the blit. Nope has a single frame and animates DMC-style by
    // alternating flipped/unflipped rapidly (a head-shake); otherwise it's the walk facing.
    bool mirrored() const
    {
        if ((reacting() ? overlay_ : base_) == Anim::Nope) return flip(0.15f);
        return facing_;
    }

    int frame() const
    {
        Anim a = reacting() ? overlay_ : base_;
        switch (a) {
            case Anim::Idle:   return flip(stepSecs_) ? FRM_IDLE2 : FRM_IDLE1;
            case Anim::Happy:  return FRM_HAPPY;
            case Anim::Angry:  return FRM_ANGRY;
            case Anim::Train:  return flip(0.35f) ? FRM_TRAIN2 : FRM_TRAIN1;
            case Anim::Attack: return flip(0.3f)  ? FRM_ATK2   : FRM_ATK1;
            case Anim::Eat:    return flip(0.4f)  ? FRM_EAT2   : FRM_EAT1;
            case Anim::Nope:   return FRM_NOPE;
            case Anim::Nap:    return flip(1.2f)  ? FRM_NAP2   : FRM_NAP1;
            case Anim::Sick:   return FRM_SICK;
            case Anim::Lose:   return FRM_LOSE;
        }
        return FRM_IDLE1;
    }

private:
    // Square wave on the shared clock: false for the first half of each period.
    bool flip(float halfPeriod) const
    {
        return ((int)(t_ / halfPeriod)) & 1;
    }

    Anim  base_        = Anim::Idle;
    Anim  overlay_     = Anim::Idle;
    bool  facing_      = false;
    float overlayLeft_ = 0.0f;
    float t_           = 0.0f;
    float stepSecs_    = ANIM_STEP_SECS;
};
