#include "training.hpp"

// Mirrors the original SceneRun::award() gate exactly, just generalized to an arbitrary
// stat table so every minigame shares one code path.
TrainingResult grant_training(Pet& pet, float energyCost,
                              const StatGain* gains, int n, int friendshipBonus)
{
    TrainingResult r{};

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

    pet.markSaved();                             // persist the session's gains
    return r;
}
