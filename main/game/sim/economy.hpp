#pragma once
#include <cstdint>
#include "items.hpp"   // ItemKind: an inventory slot is tagged with what it holds

class SaveStore;

// The player's money and bag. Design in docs/economy-and-inventory.md.
//
// PERSISTENCE: everything here lives in its OWN NVS keys, never in PetState. Adding a field
// to that blob changes its layout, which bumps PET_VERSION and WIPES the player's pet -- the
// same reason the bond allowance, mood and vitals are all kept outside it.
//
// Bits and the bag SURVIVE DEATH (rule 6): they belong to the player, not to the creature,
// so the successor inherits what you were carrying. Only per-creature state (the wish, later)
// is cleared with a new egg.

// Capped so the UI never has to reflow for a seventh digit.
constexpr uint32_t BITS_MAX = 999999;

// 48 slots is comfortably more than the base game can fill (six foods, a handful of
// medicines, a few toys) while staying ~1.3 KB -- small enough for one NVS blob.
constexpr int INV_MAX = 48;

// One stack. `id` is resolved against ItemRegistry FIRST and FoodRegistry second, which is
// what lets one bag hold both without foods being rewritten into the item schema.
struct InvSlot {
    char     id[24];
    uint8_t  kind;      // ItemKind (ITEM_FOOD for a food id)
    uint16_t count;
};

class Economy {
public:
    void init(SaveStore& save);      // load wallet + bag; call once from App::init

    // --- wallet ---------------------------------------------------------------------
    uint32_t bits() const { return bits_; }
    bool     canAfford(uint32_t n) const { return bits_ >= n; }
    void     earn(uint32_t n);       // clamped at BITS_MAX
    bool     spend(uint32_t n);      // false and NO deduction if short

    // --- bag ------------------------------------------------------------------------
    int  count(const char* id) const;                       // how many held (0 if none)
    bool add(const char* id, uint8_t kind, int n = 1);       // false if the bag is full
    bool take(const char* id, int n = 1);                    // false if not enough held
    int  slotCount() const { return n_; }
    const InvSlot& slotAt(int i) const { return slots_[i]; }

    // Buy `n` of `id` at `unit` Bits each: spends and adds, or does NEITHER. One call so a
    // caller cannot half-succeed -- taking the money and finding the bag full would be a
    // uniquely infuriating bug to hit on a device with no refunds.
    bool buy(const char* id, uint8_t kind, uint32_t unit, int n = 1);

    // Persist iff something changed since the last flush. Mutators only touch RAM: a commit
    // is a potential multi-ms flash stall on the render thread, and Bits move several times
    // per minigame. Mirrors ConversationSystem's memDirty_/flushMemory pattern; call it
    // wherever Pet::markSaved() is called.
    void flush();
    bool dirty() const { return dirty_; }

private:
    SaveStore* save_ = nullptr;
    uint32_t   bits_ = 0;
    InvSlot    slots_[INV_MAX]{};
    int        n_ = 0;
    bool       dirty_ = false;

    int  find(const char* id) const;   // slot index, or -1
    void load();
};

// --- payout rates ----------------------------------------------------------------------
// First guesses, sized against the day-shapes in docs/economy-and-inventory.md 2: a casual
// day (one session, ~4 minigames) lands around 80 Bits, an engaged one with battles nearer
// 300. They live here as named constants for E1; the doc puts them in
// flash_gamedata/config/economy.json alongside the vitals tuning, which is a small loader
// still to be written -- until then, changing a rate means a rebuild.
namespace econ {

// A full-value training session. Scaled by the energy actually spent, so a tired pet that
// trains at half value earns half -- income is metered by the energy gate that already
// exists, and needs no daily cap of its own (design rule 4).
constexpr uint32_t kTrainingSession = 20;

// Battle wins are rare by nature, so they pay well and are not metered at all -- the same
// argument that lets BOND_MILESTONE bypass the daily bond allowance.
constexpr uint32_t kBattleWin = 50;

}  // namespace econ
