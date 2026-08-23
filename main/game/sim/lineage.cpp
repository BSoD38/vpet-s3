#include "lineage.hpp"
#include "save.hpp"
#include "esp_log.h"
#include <cstring>

static const char* TAG = "LINEAGE";

// Storage: a fixed-size array blob plus a count key, because SaveStore::loadBlob only
// accepts exact-size hits (a variable-length blob would fail to load the moment a record
// was added). ~1 KB of NVS for the full ledger; newest record is at index count-1.
static const char* K_RECS = "lin";
static const char* K_CNT  = "linct";
static const char* K_GEN  = "gen";

struct Ledger { LineageRecord r[LINEAGE_MAX]; };

static int load_ledger(const SaveStore& save, Ledger& l)
{
    int n = (int)save.loadU32(K_CNT, 0);
    if (n <= 0) return 0;
    if (n > LINEAGE_MAX) n = LINEAGE_MAX;
    if (!save.loadBlob(K_RECS, &l, sizeof l)) return 0;   // count without blob: corrupt -> empty
    return n;
}

int lineage_count(const SaveStore& save)
{
    int n = (int)save.loadU32(K_CNT, 0);
    return n < 0 ? 0 : (n > LINEAGE_MAX ? LINEAGE_MAX : n);
}

bool lineage_get(const SaveStore& save, int i, LineageRecord* out)
{
    Ledger l{};
    int n = load_ledger(save, l);
    if (i < 0 || i >= n || !out) return false;
    *out = l.r[n - 1 - i];               // 0 = most recent
    return true;
}

void lineage_append(SaveStore& save, const LineageRecord& rec)
{
    Ledger l{};
    int n = load_ledger(save, l);
    if (n >= LINEAGE_MAX) {              // full: the oldest generation makes room
        memmove(&l.r[0], &l.r[1], sizeof(LineageRecord) * (LINEAGE_MAX - 1));
        n = LINEAGE_MAX - 1;
    }
    l.r[n++] = rec;
    save.beginBatch();
    save.storeBlob(K_RECS, &l, sizeof l);
    save.storeU32(K_CNT, (uint32_t)n);
    save.endBatch();
    ESP_LOGW(TAG, "recorded: gen %u %s '%s', lived %ud, bond %u, cause %u",
             (unsigned)rec.generation, rec.speciesId,
             rec.nickname[0] ? rec.nickname : "-",
             (unsigned)(rec.ageSecs / 86400u), (unsigned)rec.friendship, (unsigned)rec.cause);
}

uint32_t lineage_generation(const SaveStore& save)
{
    uint32_t g = save.loadU32(K_GEN, 1);
    return g < 1 ? 1 : g;
}

void lineage_bump_generation(SaveStore& save)
{
    save.storeU32(K_GEN, lineage_generation(save) + 1);
}
