#pragma once
#include <cstdint>
#include "personality.hpp"   // DriftAxis/AX_COUNT: foods nudge the canonical axes directly

// A data-driven food roster. Feeding is the action the player performs most, so making
// it a CHOICE turns the biggest routine duty into a channel for self-expression -- each
// food nudges the creature's temperament a different way. Design rules and the full
// rationale are in docs/food-and-feeding.md.
//
// Base foods ship in the read-only `gamedata` partition: either one directory per food
// (/gamedata/foods/<id>/food.json) or a pack (/gamedata/foods/foods.json holding an ARRAY
// of foods -- one file open instead of N at boot). SD-card foods (/sdcard/foods/, same two
// layouts) are additive and override a base food on an id collision -- the same modding
// contract as creatures.

constexpr int FOOD_MAX_TAGS = 4;

struct Food {
    char     id[24];
    char     name[24];
    // One-line flavour text. This is the ONLY hint the player gets about what a food
    // does to temperament: drift is deliberately hidden (docs/conversations-and-
    // personality.md 2.6), so effects must read from theme, never from printed numbers.
    char     desc[40];
    char     tags[FOOD_MAX_TAGS][16];
    uint8_t  tagCount;
    int16_t  fills;              // hunger restored (0..100 scale)
    int16_t  happiness;          // happiness delta
    int16_t  health;             // health delta (greens/herbal +, sweets -)
    uint16_t color;              // rgb565 icon swatch (stands in until food sprites exist)
    float    drift[AX_COUNT];    // personality nudge, indexed by DriftAxis
    // --- Reserved for the deferred economy. Parsed and stored so that gating food
    //     behind money later is a behaviour change only, with no schema break and no
    //     mod file rewrites. Ignored in v1: every food is always available. ---
    uint16_t cost;
    char     rarity[12];
};

class FoodRegistry {
public:
    static const int MAX = 32;

    void loadAll();                       // mount gamedata, scan flash + SD
    int  count() const { return count_; }
    int  indexOf(const char* id) const;   // registry index for an id, or -1
    const Food& at(int i) const { return list_[i]; }

    // True if food `i` matches `key` by either its id or one of its tags. Preferences
    // (creature.json food.likes/dislikes) match on this so a creature that "likes
    // sweet" also likes a sweet food added by a mod it has never heard of.
    bool matches(int i, const char* key) const;

private:
    Food list_[MAX];
    int  count_ = 0;

    int  upsert(const char* id);
    void scanRoot(const char* root, const char* srcTag);
    void scanPack(const char* root, const char* srcTag);   // <root>/foods.json (array)
    void parseEntry(cJSON* root, Food& f);                 // one food object -> Food
    bool parseFile(const char* path, Food& f);
    void addBuiltinFood();                // safety net if no data files are readable
};
