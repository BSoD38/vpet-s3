#pragma once
#include <cstdint>
#include "engine/display.hpp"   // LGFX_Sprite, display

// A data-driven creature roster. Each creature is a distinct species (a node in the
// evolution tree) loaded from a directory: <root>/<id>/creature.json + a sprite PNG.
// Base creatures live in the flash `creatures` partition (mounted /creatures); extra
// creatures may be dropped on the SD card (/sdcard/creatures). On an id collision the
// SD copy wins (so the card can override/mod the base game).

// One evolution edge + the gate that unlocks it. Gates test EFFECTIVE stats (base+mod).
struct EvoEdge {
    char     to[24];             // target creature id (resolved to an index at load)
    int      toIdx;              // resolved registry index, or -1 if the target is missing
    uint32_t minHp;
    uint16_t minStr, minEnd, minAgi, minInt;
    uint16_t minFriendship;
    uint8_t  maxCareMistakes;    // 255 = ignore (an always-eligible fallback edge)
};

// One creature = one node in the evolution tree.
struct Creature {
    char     id[24];
    char     name[24];
    uint8_t  tier;               // LifeStage
    uint32_t baseHp;             // innate base stats (effective = base + trained modifier)
    uint16_t baseStr, baseEnd, baseAgi, baseInt;
    float    hungerPerHr, happyPerHr, poopIntervalS;
    uint8_t  sleepStart, sleepEnd;
    float    minStageSecs;       // min time as this creature before it may evolve
    EvoEdge  evos[4];
    uint8_t  evoCount;
    char     spriteFile[24];     // sprite filename from the config (e.g. "sprite.png")
    char     spritePath[104];    // resolved absolute path to the sprite (flash or SD)
    LGFX_Sprite* sprite;         // decoded PNG in PSRAM (lazy; nullptr until first shown / after eviction)
    uint32_t spriteTick;         // LRU timestamp of last access (for eviction)
    uint8_t  spriteMiss;         // 1 = decode already failed, don't keep retrying
};

class CreatureRegistry {
public:
    static const int MAX = 40;
    static const int SPRITE_CACHE = 16;    // max decoded sprites kept resident (LRU-evicted)

    void loadAll();                        // mount flash, scan flash + SD, resolve edges (sprites are lazy)
    int  count() const { return count_; }
    int  indexOf(const char* id) const;    // registry index for an id, or -1
    const Creature& at(int i) const { return list_[i]; }

    // Sprite for a creature, decoded on first use and cached (LRU); nullptr if none.
    // Cheap to call every frame: a cache hit just bumps the LRU timestamp.
    LGFX_Sprite* sprite(int idx);

private:
    Creature list_[MAX];
    int      count_ = 0;
    int      loadedSprites_ = 0;           // how many entries currently hold a decoded sprite
    uint32_t spriteClock_ = 0;             // monotonically increasing LRU clock

    int  upsert(const char* id);           // find-or-append; returns index (or -1 if full)
    void scanRoot(const char* root, const char* srcTag);
    bool parseFile(const char* path, Creature& c);
    void resolveEdges();
    void addBuiltinEgg();                  // safety net if no data files are readable
};
