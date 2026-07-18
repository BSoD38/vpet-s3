#pragma once
#include <cstdint>

struct PetState;   // defined in pet.hpp

void save_init(void);   // init NVS (idempotent; call once early, before any task uses it)

// Persistence for the pet blob + small standalone flags (NVS namespace "pet").
class SaveStore {
public:
    bool    load(PetState& s) const;          // true if a valid blob was loaded
    void    store(const PetState& s) const;   // persist the blob
    uint8_t loadU8(const char* key, uint8_t def) const;  // small flags kept out of the blob
    void    storeU8(const char* key, uint8_t v) const;
    void    loadStr(const char* key, char* out, int n, const char* def) const;  // string kept out of the blob
    void    storeStr(const char* key, const char* v) const;
};
