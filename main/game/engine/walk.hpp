#pragma once
#include <cmath>
#include "esp_random.h"
#include "engine/anim.hpp"   // ANIM_STEP_SECS -- the walk cadence everything here derives from

// Ground locomotion and idle wandering for a creature on screen: where it stands along the
// ground, which way it faces, when it stops and turns, and the small body lift of each
// footfall.
//
// THE INVARIANT: everything here is derived from the walk-cycle cadence, never from a clock
// of its own. On the DMC sheets the two Idle frames are a walk cycle, so the creature covers
// exactly WALK_STRIDE px per Idle frame -- two independent clocks (one flipping the legs, one
// sliding the body) would agree only by coincidence and the feet would visibly skate. The
// wander director follows the same rule: it spends its acts in FOOTFALLS rather than seconds,
// and every decision lands on a footfall, so stops and turns fall on a step instead of
// mid-stride. Retune ANIM_STEP_SECS and the walk speed, the bob and the pacing all follow.
//
// What this class does NOT decide is whether the creature is allowed to move, or which pose
// it strikes when it stops. The caller passes `stepping`/`travelling` (see the update()
// comment) and reads takeRestCue() to hang an optional flourish on a stop, because poses
// belong to CreatureAnim and the reasons a creature can't walk belong to the scene.

// Distance covered per footfall. With ANIM_STEP_SECS this is the whole speed model:
// 8 px / 0.45 s ~= 18 px/s, so a creature ambles across the walkable ground in ~10 s.
// Long enough to read as a step on a 48px body, slow enough not to look hurried.
constexpr float WALK_STRIDE = 8.0f;
// Peak body rise while covering ground, at mid-frame. This is the creature's ONLY vertical
// motion: exactly one rise-and-fall per animation frame, phase-locked to it, so the body can
// never be seen bobbing against the legs. (It replaced a free-running "breathing" bob whose
// ~3.1 s period was unrelated to the cadence -- which is precisely how a bob desyncs.)
// Up-only, hitting 0 at every frame boundary, so the feet stay planted on the ground -- and
// note that PET_FEET in the home scene is placed for an up-only bob, so flipping the sign
// here would re-open the hover it was corrected for. NB the flying/swimming sprites
// (airdramon, birdramon, seadramon,
// whamon) carry their own bob baked into the sheet cell -- differing bottom padding between
// their two Idle frames -- and this rides on top of it, which is why frames must never be
// ink-trimmed to the ground: that would flatten the artist's flap.
constexpr float WALK_LIFT   = 3.0f;
// Wander pacing, in footfalls. A walk act is long enough to cross a good part of the ground
// (6..15 steps ~ 48..120 px); a rest is short, because a flourish played over it stretches it
// further (the director freezes while a reaction is on screen).
constexpr int WALK_STEPS_MIN = 6,  WALK_STEPS_RAND = 10;
constexpr int REST_STEPS_MIN = 2,  REST_STEPS_RAND = 5;
// Chance (%) of turning on the spot as it stops, and again as it sets off.
constexpr int TURN_PCT_STOP  = 40, TURN_PCT_GO = 35;

class CreatureWalk {
public:
    // Walkable span for the sprite's CENTRE (the caller insets it by the sprite's half
    // width, which varies per creature and is only known once the sprite has decoded).
    // The creature is placed at the middle of the span the first time this is called.
    void setSpan(int minX, int maxX)
    {
        if (maxX < minX) { minX = maxX = (minX + maxX) / 2; }   // sprite wider than the span
        minX_ = (float)minX;
        maxX_ = (float)maxX;
        if (!placed_) { x_ = (minX_ + maxX_) * 0.5f; placed_ = true; }
        else          { x_ = x_ < minX_ ? minX_ : (x_ > maxX_ ? maxX_ : x_); }
    }

    // --- chasing -------------------------------------------------------------------------
    // Head for a spot instead of wandering, for as long as the caller keeps asking. Only the
    // DIRECTION is taken over: the stride, the speed and the bob still come from the walk
    // cadence, so a creature chasing a ball moves exactly as fast as one ambling and the feet
    // cannot skate. The wander director is suspended meanwhile (no rests, no random turns)
    // and resumes untouched the moment the target is cleared.
    void chase(float x) { target_ = x; chasing_ = true; }
    void clearTarget()  { chasing_ = false; }
    bool chasing() const { return chasing_; }

    // Close enough to have arrived. One stride of tolerance, because travel is quantised to
    // the cadence -- asking for exactness would leave the creature jittering either side of
    // the target forever.
    bool atTarget() const { return chasing_ && fabsf(x_ - target_) <= WALK_STRIDE; }

    // `stepping`   = the walk cycle is the pose on screen at all (a nap / sick / eating /
    //                refusing creature is showing something else, and must not be walked).
    // `travelling` = the creature is also ALLOWED to cover ground (not an egg, not being
    //                petted, not sulking).
    // Both must hold for the wander director to run, so a sleeping or upset creature neither
    // paces nor strikes poses.
    // `stepSecs` is the animation's LIVE footfall period. Travel is WALK_STRIDE per footfall
    // by definition, so passing the live value is what makes the speed follow the gait: stretch
    // it for age and the creature ambles, compress it to run and it sprints, and in both cases
    // the legs and the ground cover the same distance per frame. Deriving this from the
    // ANIM_STEP_SECS *constant* instead -- as it did originally -- meant a retimed gait moved
    // its legs at one speed and its body at another, which is exactly the skate the invariant
    // above exists to prevent. It was already visible on elderly creatures.
    void update(float dt, bool stepping, bool travelling, float stepPhase, float stepSecs)
    {
        const float speed = WALK_STRIDE / (stepSecs > 0.05f ? stepSecs : 0.05f);
        bool footfall = stepPhase < phase_;      // phase wraps to 0 exactly on a frame change
        phase_ = stepPhase;
        bool active = stepping && travelling;

        if (chasing_) {
            // A creature that cannot travel still cannot move, chase or no chase -- the same
            // gate as wandering, so being asleep or upset stops the game rather than being a
            // special case the caller has to remember.
            walking_ = active && !atTarget();
            if (!walking_) return;
            dir_ = (target_ > x_) ? 1 : -1;
            x_ += dir_ * speed * dt;
            if (x_ < minX_) x_ = minX_;
            if (x_ > maxX_) x_ = maxX_;
            return;
        }

        if (active && footfall && --actSteps_ <= 0) nextAct();
        walking_ = active && !resting_;
        if (!walking_) return;

        x_ += dir_ * speed * dt;
        // Reaching the edge of the walkable ground turns the creature AND buys it a rest, so
        // it reads as "got to the fence, had a look, wandered back" rather than as a sprite
        // bouncing off an invisible wall.
        if (x_ <= minX_ || x_ >= maxX_) {
            x_   = (x_ <= minX_) ? minX_ : maxX_;
            dir_ = -dir_;
            beginRest();
        }
    }

    float x() const           { return x_; }
    // The walkable span, so anything else that lives on this ground (a ball) can be kept
    // inside the same bounds the creature can actually reach.
    float minSpan() const     { return minX_; }
    float maxSpan() const     { return maxX_; }
    bool  facingRight() const { return dir_ > 0; }
    bool  walking() const     { return walking_; }
    bool  resting() const     { return resting_; }

    // True ONCE at the start of each rest, for the scene to hang an optional idle flourish
    // on. Which pose (if any) is the scene's call, not this class's. Consumed on read.
    bool takeRestCue() { bool c = restCue_; restCue_ = false; return c; }

    // Vertical offset for the body this frame, in px, negative = up. Peaks at mid-frame and
    // returns to 0 at each frame boundary, so it reads as weight going onto a leg.
    //
    // Keyed on travel, NOT on the pose: a bob is what a body does while covering ground, so a
    // creature standing still holds its height even though its two walk frames keep
    // alternating (that alternation is CreatureAnim's clock and is untouched here -- a resting
    // creature still shifts its weight on the spot, it just doesn't rise). Poses that aren't
    // the walk cycle at all -- nap, sick, eat, refuse -- likewise get nothing, since there is
    // no footfall to sync to and an unsynced bob is the thing this exists to avoid.
    //
    // The director's own stops land on a footfall, where this is already 0, so they can't pop
    // the sprite. A stop forced mid-stride (a finger landing on the creature) can drop it by
    // up to WALK_LIFT at once, which at 3px is well under notice.
    int lift() const
    {
        if (!walking_) return 0;
        return -(int)(WALK_LIFT * sinf(phase_ * 3.14159265f) + 0.5f);
    }

private:
    static int roll(int n) { return (int)(esp_random() % (uint32_t)n); }

    void beginRest()
    {
        resting_  = true;
        actSteps_ = REST_STEPS_MIN + roll(REST_STEPS_RAND);
        restCue_  = true;
    }

    // Alternate wandering with standing still, turning on some of the transitions. Turns are
    // deliberately folded into the stops rather than given an act of their own: there are no
    // turn frames on the sheet, so a facing flip mid-walk would read as the sprite sliding
    // backwards, while one that happens while stopped reads as the creature looking around.
    void nextAct()
    {
        if (resting_) {
            if (roll(100) < TURN_PCT_GO) dir_ = -dir_;
            resting_  = false;
            actSteps_ = WALK_STEPS_MIN + roll(WALK_STEPS_RAND);
        } else {
            if (roll(100) < TURN_PCT_STOP) dir_ = -dir_;
            beginRest();
        }
    }

    float x_        = 0.0f;
    float minX_     = 0.0f;
    float maxX_     = 0.0f;
    float phase_    = 0.0f;
    int   dir_      = -1;      // DMC sprites face left natively, so start unmirrored
    int   actSteps_ = 1;       // footfalls left in this act; 1 = re-roll on the first step
    bool  resting_  = false;
    bool  restCue_  = false;
    bool  walking_  = false;   // advancing along the ground (also gates the bob)
    bool  chasing_  = false;   // heading for target_ instead of wandering (see chase())
    float target_   = 0.0f;
    bool  placed_   = false;   // has setSpan() chosen a starting position yet
};
