#pragma once
#include "core/scene.hpp"
#include "ui/widgets.hpp"

// "Sorting shift" -- the PET-LESS minigame (docs/economy-and-inventory.md 4).
//
// The one thing in the game you can do while the creature is ill, asleep or frozen, and the
// floor under priced treatment: conditionBlocked() shuts every other income source, so without
// this a sick pet plus an empty wallet would be a lockout with no way out. It therefore takes
// NO energy, trains NOTHING, and never touches the pet -- it pays Bits and nothing else.
//
// The mechanic is categorisation rather than reflex, because all five training games already
// occupy the reflex space (Run dodges, Smash taps, Bulwark times, Stance tilts, Mind Maze
// remembers). Parcels ride a belt toward the chute; the one at the front carries a shape, and
// you tap the bin that takes that shape. The bins SHUFFLE -- at the start of every shift and
// again as the pace picks up -- so the skill is reading the label, never muscle memory for a
// position.
//
// ENDLESS, not timed. A fixed forty seconds made every shift identical and there was nothing
// to get better at; running until you drop three parcels gives it a ceiling worth reaching
// for. The payout is still capped, so a superb shift is a good hour's pocket money rather than
// a career -- this is a floor, not an income source.
class SceneWork : public Scene {
public:
    void onEnter() override;
    void update(float dt) override;
    void render() override;
    void onInput(const Input& in) override;

    // Timed and active, exactly like the training minigames: a nap or an idle timeout must
    // not interrupt a shift.
    bool allowsSleep() const override { return false; }
    // ...but unlike them, the care sim keeps running. Those freeze it so a minigame cannot be
    // used to park the pet; here the pet is usually ill or asleep anyway, and freezing decay
    // during a shift would make working a way to dodge the clock.
    float careSpeed() const override { return 1.0f; }

private:
    enum Phase { READY, WORKING, OVER };

    static const int SHAPE_N  = 3;                // circle / square / triangle
    static const int BELT_MAX = 6;                // parcels queued on the belt at once
    static const int LIVES    = 3;                // dropped/mis-sorted parcels before the shift ends
    static const int PER_LEVEL = 6;               // sorts between difficulty steps
    static const int BITS_CAP  = 25;              // ceiling on one shift's pay

    struct Parcel { uint8_t shape; float x; bool used; };

    Phase    phase_ = READY;
    Parcel   belt_[BELT_MAX];
    uint8_t  binShape_[SHAPE_N];                  // which shape each bin takes, right now
    int      head_ = 0, tail_ = 0;
    float    spawnT_ = 0.0f;
    int      score_ = 0;                          // correctly sorted
    int      missed_ = 0;                         // wrong bin, or fell off the end
    int      level_ = 0;                          // difficulty step (drives speed + cadence)
    uint32_t bits_ = 0;
    float    doneT_ = 0.0f;
    float    flashT_ = 0.0f;                      // >0 = feedback on the last sort
    bool     flashGood_ = false;
    float    swapT_ = 0.0f;                       // >0 = "BINS SWAPPED" banner showing
    bool     paid_ = false;                       // guards double payment on the result frame

    void shuffleBins();
    void spawn();
    void sortInto(int bin);
    void miss();
    void finish();
    float beltSpeed() const;
    float spawnGap() const;
};
