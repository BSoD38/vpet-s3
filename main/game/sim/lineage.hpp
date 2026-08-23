#pragma once
#include <cstdint>

class SaveStore;

// The lineage ledger (docs/death-and-lifespan.md §7): one compact record per death,
// persisted FROM DAY ONE so the future cemetery scene can render generation 1 even though
// it doesn't exist yet. The journal's "In Memory" tab renders from this now.
//
// Records live OUTSIDE the pet blob and survive new eggs -- they are the family history,
// not the individual. Only a factory reset clears them.
//
// The record is written the moment a brink resolves to death (BEFORE the scene plays a
// frame), so a power cut mid-farewell finds the death already in the ledger.

struct LineageRecord {
    char     speciesId[24];   // registry id at death (resolves to name/sprite if still installed)
    char     nickname[20];    // player-given name ("" = none)
    uint32_t ageSecs;         // how long it lived
    uint32_t diedAt;          // RTC seconds
    uint16_t friendship;      // the bond it died with
    uint8_t  stage;           // LifeStage reached
    uint8_t  cause;           // Brink value that ended it (old age / critical)
    uint8_t  savesUsed;       // miracles it was granted along the way
    uint8_t  generation;      // 1-based
    uint8_t  pad[2];
};

constexpr int LINEAGE_MAX = 16;   // oldest drops past this; 16 year-long lives is decades

int  lineage_count(const SaveStore& save);
// i = 0 is the MOST RECENT death. Returns false past the end.
bool lineage_get(const SaveStore& save, int i, LineageRecord* out);
void lineage_append(SaveStore& save, const LineageRecord& rec);

// The CURRENT pet's generation, 1-based (a fresh install is generation 1).
uint32_t lineage_generation(const SaveStore& save);
void     lineage_bump_generation(SaveStore& save);   // the line continues: +1, persisted
