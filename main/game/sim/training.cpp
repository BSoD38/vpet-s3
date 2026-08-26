#include "training.hpp"

// Mirrors the original SceneRun::award() gate exactly, just generalized to an arbitrary
// stat table so every minigame shares one code path.
TrainingResult grant_training(Pet& pet, Economy& econ, float energyCost,
                              const StatGain* gains, int n, int friendshipBonus)
{
    TrainingResult r{};

    // Belt-and-braces: the Activities menu is closed while the care freeze is on, so no
    // minigame should ever reach this. If one does, it awards nothing -- a paused creature
    // that could still be trained would be a way to grow stats with the clock stopped.
    if (pet.frozen()) return r;

    float have  = pet.energy();
    float cost  = energyCost < 0.0f ? 0.0f : energyCost;
    float ratio = (cost <= 0.0f) ? 1.0f : (have >= cost ? 1.0f : have / cost);
    r.tired = ratio < 0.999f;

    for (int i = 0; i < n; i++) {
        if (gains[i].amount <= 0) continue;
        int amt = (int)(gains[i].amount * ratio);
        if (amt <= 0) continue;
        r.granted[gains[i].stat] += amt;
        pet.trainStat(gains[i].stat, (uint32_t)amt);
    }

    r.energySpent = have < cost ? have : cost;   // spend the cost, or drain what's left if short
    pet.spendEnergy(r.energySpent);

    if (friendshipBonus != 0) {
        pet.addFriendship(friendshipBonus);
        r.friendship = friendshipBonus;
    }

    // Pay for the session. A frozen pet returned above, so a paused creature cannot be
    // farmed for Bits any more than it can be farmed for stats.
    if (cost > 0.0f) {
        r.bits = (uint32_t)(econ::kTrainingSession * (r.energySpent / cost));
    } else {
        r.bits = econ::kTrainingSession;         // a game with no energy cost still pays
    }
    econ.earn(r.bits);

    pet.markSaved();                             // persist the session's gains
    econ.flush();                                // ...and the Bits, in the same breath
    return r;
}
