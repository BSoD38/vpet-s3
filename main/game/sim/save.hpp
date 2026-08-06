#pragma once
#include <cstdint>

struct PetState;   // defined in pet.hpp

void save_init(void);   // init NVS (idempotent; call once early, before any task uses it)

// Persistence for the pet blob + small standalone flags (NVS namespace "pet").
class SaveStore {
public:
    // Batch several store* calls into ONE nvs open/commit/close. Every save burst in the
    // game writes 3-7 keys back-to-back (pet blob + bond + mood + drift, or the three
    // conversation-memory blobs plus their headers); without batching each key paid its
    // own commit, and a commit is a potential multi-ms flash stall on the render thread.
    // Loads are unaffected. Not reentrant: don't nest batches.
    void beginBatch() const;
    void endBatch() const;

    bool    load(PetState& s) const;          // true if a valid blob was loaded
    void    store(const PetState& s) const;   // persist the blob
    uint8_t loadU8(const char* key, uint8_t def) const;  // small flags kept out of the blob
    void    storeU8(const char* key, uint8_t v) const;
    // Wider counters kept out of the blob for the same reason as the flags above:
    // adding a field to PetState changes the blob layout, which bumps PET_VERSION and
    // WIPES the player's pet. Anything new goes in its own key instead.
    uint32_t loadU32(const char* key, uint32_t def) const;
    void     storeU32(const char* key, uint32_t v) const;
    // Arbitrary blob under its own key, for state that wants its own versioning independent
    // of PetState (e.g. the personality drift state -- revising it must never wipe the pet,
    // and resetting the pet must never erase who the creature became).
    bool loadBlob(const char* key, void* out, unsigned size) const;   // true only on an exact-size hit
    void storeBlob(const char* key, const void* data, unsigned size) const;
    void    loadStr(const char* key, char* out, int n, const char* def) const;  // string kept out of the blob
    void    storeStr(const char* key, const char* v) const;

    // Factory reset: erase the ENTIRE NVS partition (pet, drift, conversations, nickname,
    // tower floor, settings -- everything player-owned lives there) and restart the chip.
    // Does not return. The next boot finds empty storage and starts a new game.
    [[noreturn]] void factoryReset() const;

private:
    // Write-handle plumbing for beginBatch()/endBatch(). uint32_t IS nvs_handle_t; kept
    // untyped here so this header needn't pull in nvs.h.
    bool acquire(uint32_t& h) const;   // batch handle, or a fresh open
    void release(uint32_t h) const;    // commit+close, unless inside a batch

    mutable uint32_t batch_     = 0;
    mutable bool     batchOpen_ = false;
};
