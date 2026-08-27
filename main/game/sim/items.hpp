#pragma once
#include <cstdint>
#include "personality.hpp"   // DriftAxis/AX_COUNT: toys nudge the canonical axes, exactly as foods do
#include "vitals.hpp"        // Condition: a `care` item declares which one it treats

// The non-food item roster: toys, decor, medicine, one-off specials and keepsakes.
// Design and rationale in docs/economy-and-inventory.md.
//
// This deliberately MIRRORS FoodRegistry rather than absorbing it. Foods keep their own
// schema, their own folder and their own registry, so nothing a mod author already shipped
// had to be rewritten when the economy landed; the inventory (sim/economy.hpp) is what
// holds both, resolving an id against ItemRegistry first and FoodRegistry second.
//
// Base items ship in the read-only `gamedata` partition -- one directory per item
// (/gamedata/items/<id>/item.json) or a pack (/gamedata/items/items.json holding an ARRAY).
// Mod packs and then loose SD files (/sdcard/items/) overlay it, later wins, an id collision
// overrides. Same three-layer contract as creatures and foods.

// MODDING CEILING (pinned for E6, docs/economy-and-inventory.md 5.1): `kind` below is a CLOSED
// enum whose behaviour is compiled in, so a mod can add more toys or more medicine but cannot
// add a new kind of thing, combine effects, or gate an item on anything. The fix is to replace
// the single-purpose fields with a composable `effects` list plus a `when` gate, borrowing the
// conversation system's shape. Do NOT extend this enum case-by-case in the meantime -- that is
// the path that makes the rework harder.

constexpr int ITEM_MAX_TAGS = 4;

// What an item IS, which decides what the Bag offers to do with it.
//
// ITEM_FOOD is in the enum but never in this registry: it tags an inventory slot whose id
// belongs to FoodRegistry. Keeping one kind space means the Bag can hold food and items in
// a single list without a second parallel container.
enum ItemKind : uint8_t {
    ITEM_FOOD = 0,     // resolves against FoodRegistry (never stored here)
    ITEM_TOY,          // durable; drift + happiness when played with
    ITEM_DECOR,        // durable; occupies one room slot
    ITEM_CARE,         // consumable; treats a Condition
    ITEM_SPECIAL,      // consumable; a one-off effect (evolution catalyst, token of amends)
    ITEM_KEEPSAKE,     // durable; memorial, never purchasable
    ITEM_KIND_COUNT
};

// Where a decor piece hangs. A fixed set keeps the home-scene render bounded: one item per
// slot, so the number of things drawn behind the creature can never grow with the roster.
enum DecorSlot : uint8_t {
    DSLOT_FLOOR = 0, DSLOT_WALL, DSLOT_WINDOW, DSLOT_BED, DSLOT_FEATURE, DSLOT_COUNT,
    DSLOT_NONE = 0xFF
};

// How an item is drawn while there is no sprite art. PRESENTATION ONLY -- the same class of
// field as `color`, so it is not the sort of case-by-case behaviour extension the E6 modding
// pin warns about. Toys need it because four identical circles on the floor tell the player
// nothing about which toy is out.
enum ItemShape : uint8_t { SHAPE_ROUND = 0, SHAPE_SQUARE, SHAPE_SOFT, SHAPE_BAR };

// How a toy is played with. An affordance, not an effect -- it says which control the room
// offers, while what the play DOES is still `drift` + `happiness` for every toy alike. It
// folds into the composable-effects rework at E6 (see the modding note above) rather than
// being the start of a per-toy behaviour zoo.
//   PLAY_TAP  -- tap it where it sits (the default, and every toy before the ball)
//   PLAY_TOSS -- drag and flick it; the creature runs it down and bats it back
enum ItemPlay : uint8_t { PLAY_TAP = 0, PLAY_TOSS };

// Sentinel for "this item treats no condition" -- distinct from COND_HEALTHY, which as a
// `treats` value would mean "cures being well".
constexpr uint8_t TREATS_NONE = 0xFF;

// What a care item acts on. This names a TRACK, not one exact state: a remedy has to be
// usable on a creature that has already got worse, or the only way to fix a Very Sick pet
// would be the expensive dose and the cheap one would be a trap.
//   TRACK_SICK    -> Sick and Very Sick
//   TRACK_INJURED -> Injured
enum TreatTrack : uint8_t { TRACK_SICK = 0, TRACK_INJURED };

// How far one dose moves the condition. ONE step is the default and the reason the tiers
// mean anything: two cheap doses and one expensive one reach the same place, but the cheap
// route costs a treatment cooldown in between (see Pet::treat).
enum Potency : uint8_t { POTENCY_STEP = 1, POTENCY_FULL = 2 };

struct Item {
    char     id[24];
    char     name[24];
    // One line of flavour. As with foods, this is the ONLY hint at what a toy does to
    // temperament: drift stays hidden (docs/conversations-and-personality.md 2.6), so the
    // effect has to read from theme rather than from a printed number.
    char     desc[40];
    char     tags[ITEM_MAX_TAGS][16];
    uint8_t  tagCount;
    uint8_t  kind;               // ItemKind
    uint16_t cost;               // Bits. 0 = not sold (keepsakes, quest gifts)
    char     rarity[12];         // "common"/"uncommon"/"rare" -- drives shop stock rotation
    uint16_t color;              // rgb565 swatch (stands in until item sprites exist)

    // --- kind-specific. Unused fields stay zeroed, which is why every kind can share one
    //     POD: an item is small, and a union would buy nothing but casting. ---
    int16_t  happiness;          // toys: happiness granted per play
    float    drift[AX_COUNT];    // toys: personality nudge, indexed by DriftAxis
    uint8_t  slot;               // decor: DecorSlot (DSLOT_NONE otherwise)
    uint8_t  shape;              // ItemShape: how it is drawn with no sprite
    uint8_t  play;               // toys: ItemPlay -- which control the room offers
    uint8_t  treats;             // care: TreatTrack acted on (TREATS_NONE otherwise)
    uint8_t  potency;            // care: Potency -- one step, or a full clear of the track
    int16_t  health;             // care: HP restored (a tonic treats no condition at all)
};

class ItemRegistry {
public:
    static const int MAX = 48;

    void loadAll();                       // mount gamedata, scan flash + paks + SD
    int  count() const { return count_; }
    int  indexOf(const char* id) const;   // registry index for an id, or -1
    const Item& at(int i) const { return list_[i]; }

    // True if item `i` matches `key` by either its id or one of its tags -- the same
    // contract FoodRegistry::matches offers, so a creature that "likes quiet things" can
    // like a quiet toy shipped by a mod it has never heard of.
    bool matches(int i, const char* key) const;

private:
    Item list_[MAX];
    int  count_ = 0;

    int  upsert(const char* id);
    void scanRoot(const char* root, const char* srcTag);
    void scanPack(const char* root, const char* srcTag);   // <root>/items.json (array)
    void parseEntry(cJSON* root, Item& it);
    bool parseFile(const char* path, Item& it);
};

// Display label for a kind ("Toy", "Medicine", ...), for the Bag and Shop rows.
const char* item_kind_name(uint8_t kind);
