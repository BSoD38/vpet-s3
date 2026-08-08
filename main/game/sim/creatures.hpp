#pragma once
#include <cstdint>
#include "engine/display.hpp"   // LGFX_Sprite, display

// A data-driven creature roster. Each creature is a distinct species (a node in the
// evolution tree) loaded from a directory: <root>/<id>/creature.json + a sprite PNG.
// Base creatures live in the flash `creatures` partition (mounted /creatures); extra
// creatures may be dropped on the SD card (/sdcard/creatures). On an id collision the
// SD copy wins (so the card can override/mod the base game).
//
// A creature's art is either a single pose ("frames": 1, the default) or a 16-frame
// DMC-style sheet ("frames": 16): one PNG laid out as a 4x4 grid, indices running
// left-to-right then top-to-bottom, cell size = sheetW/4 x sheetH/4. The sheet is
// carved into 16 small sprites at decode time so every existing blit helper (and the
// downscale cache) keeps working on plain per-frame sprites.

// Frame indices within a 16-frame sheet (the DMC standard order). Single-sprite
// creatures show their one pose for every index, so callers can use these freely.
enum SheetFrame : uint8_t {
    FRM_IDLE1 = 0, FRM_IDLE2, FRM_HAPPY, FRM_ANGRY,
    FRM_TRAIN1, FRM_TRAIN2, FRM_ATK1, FRM_ATK2,
    FRM_EAT1, FRM_EAT2, FRM_NOPE, FRM_EXTRA,
    FRM_NAP1, FRM_NAP2, FRM_SICK, FRM_LOSE,
    FRM_COUNT,
};

// Battle attribute for the type triangle: Vaccine > Data > Virus > Vaccine. Free is
// neutral (no advantage either way). Stored on each Creature; drives combat type bonuses.
enum Attribute : uint8_t {
    ATTR_FREE = 0,
    ATTR_VACCINE,
    ATTR_DATA,
    ATTR_VIRUS,
};

// Presentation helpers for the battle attribute (reusable across scenes/UI).
uint16_t    attr_color(uint8_t attribute);   // theme color (rgb565)
const char* attr_short(uint8_t attribute);   // 3-letter tag: "VAC"/"DAT"/"VIR"/"---"

// One evolution edge + the gate that unlocks it. Gates test EFFECTIVE stats (base+mod).
struct EvoEdge {
    char     to[24];             // target creature id (resolved to an index at load)
    int      toIdx;              // resolved registry index, or -1 if the target is missing
    uint32_t minHp;
    uint16_t minStr, minEnd, minAgi, minInt;
    uint16_t minFriendship;
    uint32_t minWins;            // battle wins required (0 = ignore)
    uint8_t  maxCareMistakes;    // 255 = ignore (an always-eligible fallback edge)
};
// 6 edges: the DMC V1 lines need 5 (4 branches + a Numemon fallback), +1 headroom.
static const int MAX_EVOS = 6;

// One creature = one node in the evolution tree.
struct Creature {
    char     id[24];
    char     name[24];
    uint8_t  tier;               // LifeStage
    uint8_t  attribute;          // Attribute (Vaccine/Data/Virus triangle; Free = neutral)
    uint32_t baseHp;             // innate base stats (effective = base + trained modifier)
    uint16_t baseStr, baseEnd, baseAgi, baseInt;
    float    hungerPerHr, happyPerHr, poopIntervalS;
    uint8_t  sleepStart, sleepEnd;
    float    minStageSecs;       // min time as this creature before it may evolve

    // --- voice (see docs/sound-engine.md) --------------------------------------------------
    // How this creature SOUNDS, in the two cheapest possible pieces. Neither costs any audio
    // content, which is the point: a roster imported by the hundred cannot be given hundreds
    // of hand-authored cries, and one where only the favourites have a voice sounds worse than
    // one where none do.
    //
    // `voiceFamily` names a shared set of authored sounds ("beast", "machine"): a creature-
    // voiced id like `pet_happy` resolves to `<id>_pet_happy`, then `<voiceFamily>_pet_happy`,
    // then plain `pet_happy`, so a handful of families cover everything and a single creature
    // can still be given a cry of its own. Empty = fall straight through to the base sound.
    //
    // `voicePitch` is the scalar that does the heavy lifting. A synthesised cry played slower
    // is also lower and longer, so one number per creature turns one sound into a whole roster
    // of them. Defaulted from tier plus a hash of the id when creature.json omits it, so every
    // creature has a distinct voice whether or not anyone ever authored one for it.
    char     voiceFamily[16];
    float    voicePitch;

    EvoEdge  evos[MAX_EVOS];
    uint8_t  evoCount;
    char     spriteFile[24];     // sprite filename from the config (e.g. "sheet.png")
    char     spritePath[104];    // resolved absolute path to the sprite (flash or SD)
    // The folder this creature was loaded from, resolved (flash, a pak, or SD). Kept because a
    // creature's assets are not only its sprite: `<dir>/sounds/` is its own sound set, loaded on
    // demand when it starts speaking (see docs/sound-engine.md). Storing the folder rather than
    // re-deriving it from spritePath means a creature with no art still has a voice.
    char     dir[104];
    uint8_t  frameCount;         // 1 (single pose) or 16 (4x4 DMC sheet)
    LGFX_Sprite* frames[FRM_COUNT];  // decoded frames in PSRAM (lazy; null until shown / after eviction)
    uint32_t spriteTick;         // LRU timestamp of last access (for eviction)
    uint8_t  spriteMiss;         // 1 = decode already failed, don't keep retrying
};

class CreatureRegistry {
public:
    // Cap on installed creatures. Cheap to raise now that the table lives in PSRAM (see list_):
    // it costs ~600 bytes of PSRAM per slot and none of the scarce internal heap. This is a CAP,
    // not a count, so an unused slot costs nothing but address space. What actually limits a big
    // roster is BOOT TIME -- every installed creature is one directory open plus one JSON parse --
    // and resolveEdges() being O(n^2) in the installed count.
    static const int MAX = 200;
    static const int SPRITE_CACHE = 16;    // max decoded sprites kept resident (LRU-evicted)

    void loadAll();                        // mount flash, scan flash + SD, resolve edges (sprites are lazy)
    int  count() const { return count_; }
    int  indexOf(const char* id) const;    // registry index for an id, or -1
    const Creature& at(int i) const { return list_[i]; }

    // Default pose for a creature (frame 0 = Idle1), decoded on first use and cached
    // (LRU); nullptr if none. Cheap to call every frame: a hit just bumps the LRU clock.
    LGFX_Sprite* sprite(int idx) { return frame(idx, FRM_IDLE1); }

    // A specific sheet frame (SheetFrame). Single-pose creatures return their one
    // sprite for any index, so animation code needs no special-casing.
    LGFX_Sprite* frame(int idx, int f);

private:
    // ~600 bytes per entry, so the whole table is ~120 KB -- far too much to sit in internal RAM
    // for something only read when a creature is looked up or drawn. Allocated once in loadAll().
    Creature* list_ = nullptr;
    int      count_ = 0;
    int      loadedSprites_ = 0;           // how many entries currently hold decoded frames
    uint32_t spriteClock_ = 0;             // monotonically increasing LRU clock

    int  upsert(const char* id);           // find-or-append; returns index (or -1 if full)
    void scanRoot(const char* root, const char* srcTag);
    bool parseFile(const char* path, Creature& c);
    void resolveEdges();
    void addBuiltinEgg();                  // safety net if no data files are readable
};
