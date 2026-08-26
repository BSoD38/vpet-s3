#include "economy.hpp"
#include "save.hpp"
#include "esp_log.h"
#include <cstring>

static const char* TAG = "ECON";

// NVS keys. Short because NVS caps a key at 15 chars, and separate because each half wants
// its own lifetime: revising the bag layout must never cost the player their Bits.
static const char* K_BITS = "bits";
static const char* K_INV  = "inv";

// The bag blob carries its own magic+version, independent of PET_VERSION, so the inventory
// schema can change without touching the pet -- and a pet reset never empties the bag.
static const uint32_t INV_MAGIC   = 0x564249;   // 'VBI'
static const uint16_t INV_VERSION = 1;

struct InvSave {
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    InvSlot  slots[INV_MAX];
};

void Economy::init(SaveStore& save)
{
    save_ = &save;
    load();
}

void Economy::load()
{
    bits_ = save_->loadU32(K_BITS, 0);
    if (bits_ > BITS_MAX) bits_ = BITS_MAX;

    InvSave blob{};
    n_ = 0;
    if (save_->loadBlob(K_INV, &blob, sizeof blob)
        && blob.magic == INV_MAGIC && blob.version == INV_VERSION) {
        n_ = blob.count <= INV_MAX ? blob.count : INV_MAX;
        memcpy(slots_, blob.slots, sizeof(InvSlot) * n_);
    }
    // A missing or stale blob is not an error: it is a new player, or a schema bump. The
    // bag simply starts empty, and the Bits (their own key) are untouched either way.
    dirty_ = false;
    ESP_LOGI(TAG, "wallet %u bits, %d stacks", (unsigned)bits_, n_);
}

void Economy::flush()
{
    if (!dirty_ || !save_) return;

    InvSave blob{};
    blob.magic   = INV_MAGIC;
    blob.version = INV_VERSION;
    blob.count   = (uint16_t)n_;
    memcpy(blob.slots, slots_, sizeof(InvSlot) * n_);

    save_->beginBatch();                 // one open/commit/close for both keys
    save_->storeU32(K_BITS, bits_);
    save_->storeBlob(K_INV, &blob, sizeof blob);
    save_->endBatch();
    dirty_ = false;
}

// --- wallet ---------------------------------------------------------------------

void Economy::earn(uint32_t n)
{
    if (n == 0) return;
    uint32_t before = bits_;
    // Saturating: BITS_MAX - bits_ can be computed safely because bits_ is clamped on load
    // and never exceeds it here, so this cannot wrap.
    bits_ = (n > BITS_MAX - bits_) ? BITS_MAX : bits_ + n;
    if (bits_ != before) dirty_ = true;
}

bool Economy::spend(uint32_t n)
{
    if (n > bits_) return false;         // no partial spends: the caller decides what to do
    bits_ -= n;
    dirty_ = true;
    return true;
}

// --- bag ------------------------------------------------------------------------

int Economy::find(const char* id) const
{
    if (!id || !*id) return -1;
    for (int i = 0; i < n_; i++)
        if (strcmp(slots_[i].id, id) == 0) return i;
    return -1;
}

int Economy::count(const char* id) const
{
    int i = find(id);
    return i < 0 ? 0 : slots_[i].count;
}

bool Economy::add(const char* id, uint8_t kind, int n)
{
    if (!id || !*id || n <= 0) return false;

    int i = find(id);
    if (i < 0) {
        if (n_ >= INV_MAX) { ESP_LOGW(TAG, "bag full; '%s' not added", id); return false; }
        i = n_++;
        memset(&slots_[i], 0, sizeof slots_[i]);
        strncpy(slots_[i].id, id, sizeof(slots_[i].id) - 1);
        slots_[i].kind = kind;
    }
    // Saturate rather than wrap. A stack that silently rolled over to 0 would look like the
    // player's whole supply vanishing.
    uint32_t total = (uint32_t)slots_[i].count + (uint32_t)n;
    slots_[i].count = (uint16_t)(total > 0xFFFF ? 0xFFFF : total);
    dirty_ = true;
    return true;
}

bool Economy::take(const char* id, int n)
{
    if (n <= 0) return false;
    int i = find(id);
    if (i < 0 || slots_[i].count < n) return false;

    slots_[i].count -= (uint16_t)n;
    if (slots_[i].count == 0) {          // drop the empty stack (swap with the last)
        slots_[i] = slots_[n_ - 1];
        n_--;
    }
    dirty_ = true;
    return true;
}

bool Economy::buy(const char* id, uint8_t kind, uint32_t unit, int n)
{
    if (!id || !*id || n <= 0) return false;

    uint32_t total = unit * (uint32_t)n;
    if (unit != 0 && total / unit != (uint32_t)n) return false;   // overflow guard
    if (!canAfford(total)) return false;

    // Check the bag BEFORE spending: a purchase that took the Bits and then found no room
    // would be unrecoverable, since nothing can be sold back.
    if (find(id) < 0 && n_ >= INV_MAX) {
        ESP_LOGW(TAG, "bag full; '%s' not bought", id);
        return false;
    }

    spend(total);
    add(id, kind, n);
    ESP_LOGI(TAG, "bought %dx '%s' for %u (%u left)", n, id, (unsigned)total, (unsigned)bits_);
    return true;
}
