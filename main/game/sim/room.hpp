#pragma once
#include <cstdint>

class SaveStore;

// The room the creature lives in: what the player has PUT OUT, as opposed to what they own
// (sim/economy.hpp) or what the creature is (sim/pet.hpp).
//
// One toy at a time in E3; E5 adds the decor slots beside it, which is why this is its own
// small type rather than a field bolted onto Economy. The room and the wallet are different
// ideas and the wallet has no business knowing what is on the floor.
//
// Persists in its own NVS keys and SURVIVES DEATH along with the bag: the successor inherits
// the room you furnished (docs/economy-and-inventory.md rule 6).
class Room {
public:
    void init(SaveStore& save);

    // Item id of the toy currently out, or "" if none. The id is stored rather than a
    // registry index because a registry index is not stable across a mod being installed or
    // removed -- the same reason the pet stores its creature by id.
    const char* toy() const { return toy_; }
    bool        hasToy() const { return toy_[0] != '\0'; }
    void        setToy(const char* id);   // nullptr or "" puts it away; persists immediately

private:
    SaveStore* save_ = nullptr;
    char       toy_[24] = {0};
};
