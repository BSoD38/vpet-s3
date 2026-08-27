#include "conversation.hpp"
#include "gamedata.hpp"
#include "save.hpp"
#include "engine/clock.hpp"      // clock_now (journal timestamps)
#include "engine/pakfs.hpp"      // mounted mod packs are extra scan roots
#include "pet.hpp"               // FRIENDSHIP_MAX (bond-scaled talk frequency)
#include "esp_heap_caps.h"       // PSRAM allocation for the active conversation
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "cJSON.h"
#include <dirent.h>
#include <sys/stat.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>

static const char* TAG = "CONV";

// --- tuning ---------------------------------------------------------------------
// Gap between offered conversations, rolled randomly in this range so the creature doesn't
// speak on a metronome. Generous on purpose: something said every couple of minutes is noise,
// while something said a few times a day is an event.
static const float COOLDOWN_MIN   = 900.0f;    // 15 min
static const float COOLDOWN_MAX   = 1800.0f;   // 30 min
// ...and the bond shortens it, so a Soulbound creature really is chattier than a stranger
// (doc 6.1). At full bond the gap is halved: ~7-15 min instead of ~15-30.
static const float COOLDOWN_BOND_CUT = 0.5f;
// Scanning is CHANGE-TRIGGERED, not polled. Walking the library costs ~9 ms per file, and the
// pet's state moves slowly, so a fixed interval spends that repeatedly to re-derive an identical
// answer -- and the waste grows with every conversation added. Instead: rescan when something a
// gate can actually test has changed, with a long fallback so nothing can wedge.
static const float FALLBACK_SECS  = 300.0f;  // safety net if a trigger is ever missed
static const float MIN_SCAN_GAP   = 3.0f;    // floor between scans; also lets boot settle first
// Files examined per frame. MEASURED on hardware: one file costs ~7-12 ms (FAT open + read +
// cJSON parse), so a budget of 6 did ~40-70 ms of work in a single frame against a ~25 ms
// frame -- and even 2 could double a frame. It MUST stay at 1: a scan simply takes more
// frames, which is invisible because nothing waits on it. (App additionally pauses scanning
// entirely in timing-critical scenes -- see update()'s allowScan.) This per-file cost is also
// the real argument for the derived index (doc 5.2): one small indexed read instead of N parses.
static const int   SCAN_BUDGET    = 1;
// Per-conversation source cap. MEASURED: a rich node (2-3 choices with effects) is ~230 bytes of
// JSON, and up to ~400 with a factNote and setMood on every choice -- so 8 KB only fitted about
// 20 dense nodes even though CONV_MAX_NODES is 32, and an oversized file is SILENTLY skipped
// (read_file returns null). 32 KB covers a full 32-node conversation with room to spare;
// tools/conv_lint.py enforces it at author time so the failure can't stay invisible.
static const long  MAX_FILE_BYTES = 32768;

static const uint32_t MEM_VERSION = 2;   // bumped: facts gained a display note, journal added
static const char*    K_FACTS     = "cfacts";     // GLOBAL: outlives the creature
static const char*    K_SEEN      = "cseen";      // per-creature history
static const char*    K_JRN       = "cjrnl";      // per-creature journal (ids + titles)

// Pool directory names, indexed by ConvPool. Species conversations live in the creature's own
// folder, so they're handled separately (see dirFor).
static const char* POOL_DIR[POOL_COUNT] = { "natures", "personalities", "player", "" };

// --- small helpers (file/JSON access shared via sim/gamedata) ----------------------

// Gate numerics arrive from hand-written JSON. Clamp instead of casting: a double->unsigned
// cast of an out-of-range value is UB, and in practice "minFriendship": 70000 would wrap to
// 4464 and gate far earlier than the author intended, with no warning anywhere.
static uint32_t jgate(cJSON* o, const char* k, double def, double lo, double hi)
{
    double v = gd_num(o, k, def);
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return (uint32_t)v;
}

// A string gate matches when it isn't specified, or equals the context value.
static bool str_gate(cJSON* w, const char* key, const char* actual)
{
    cJSON* v = cJSON_GetObjectItem(w, key);
    if (!cJSON_IsString(v) || !v->valuestring || !v->valuestring[0]) return true;
    return actual && strcmp(v->valuestring, actual) == 0;
}

static uint32_t fnv1a(const char* s)
{
    uint32_t h = 0x811C9DC5u;
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h;
}

// One rolled gap, shortened by how well the creature knows you.
static float roll_cooldown(uint16_t friendship)
{
    const float r    = (float)(esp_random() % 1001u) / 1000.0f;      // 0..1
    const float base = COOLDOWN_MIN + r * (COOLDOWN_MAX - COOLDOWN_MIN);
    const float bond = (float)friendship / (float)FRIENDSHIP_MAX;    // 0..1
    return base * (1.0f - COOLDOWN_BOND_CUT * bond);
}

static uint32_t fnv_mix(uint32_t h, uint32_t v)
{
    for (int i = 0; i < 4; i++) { h ^= (v >> (i * 8)) & 0xFFu; h *= 16777619u; }
    return h;
}

// Fingerprint of everything a `when` gate can test. When this is unchanged, a rescan is
// guaranteed to reach the same verdict, so it can be skipped entirely.
//
// Friendship is BUCKETED to 100s deliberately: the raw value moves on almost every interaction,
// which would make the fingerprint change constantly and defeat the whole point. The daily bond
// allowance is 60, so a bucket crossing is roughly a once-a-day event -- about the granularity a
// minFriendship gate cares about.
static uint32_t ctx_hash(const ConvContext& c)
{
    uint32_t h = 0x811C9DC5u;
    h = fnv_mix(h, c.friendship / 100u);
    h = fnv_mix(h, c.wins);
    h = fnv_mix(h, c.stage);
    h = fnv_mix(h, (uint32_t)c.hour);
    h = fnv_mix(h, (c.sick ? 1u : 0u) | (c.hungry ? 2u : 0u) | (c.asleep ? 4u : 0u));
    h = fnv_mix(h, fnv1a(c.nature  ? c.nature  : ""));
    h = fnv_mix(h, fnv1a(c.trait   ? c.trait   : ""));
    h = fnv_mix(h, fnv1a(c.species ? c.species : ""));
    h = fnv_mix(h, fnv1a(c.mood    ? c.mood    : ""));
    h = fnv_mix(h, fnv1a(c.life    ? c.life    : ""));
    h = fnv_mix(h, c.reprieves);
    return h;
}

// --- memory (facts + seen) --------------------------------------------------------

// Header keys carrying the counts; the arrays themselves are stored as raw blobs.
//
// These read and write the MEMBER ARRAYS IN PLACE rather than copying through a local struct.
// That matters: facts + seen + journal together are ~6.5 KB, and holding all three as stack
// locals in one function overflowed the task stack (a corrupted backtrace, crashing a few
// boots in a row). In-place transfer costs no stack at all.
//
// A blob whose stored size doesn't match the current struct is rejected by loadBlob, so
// changing any of these layouts safely discards the old data instead of misreading it.
static const char* K_VER    = "cver";
static const char* K_FACTN  = "cfactn";
static const char* K_SEENH  = "cseenh";    // (count << 16) | head
static const char* K_JRNH   = "cjrnlh";    // (count << 16) | head
// RTC second before which no new conversation may be offered. Persisted so the cooldown
// survives a reboot: a RAM-only cooldown made "reboot the device" a fresh offer ~3 s after
// boot -- a grind loop for repeatable conversations.
static const char* K_NEXT   = "cvnext";
// Hash of the last conversation played through. Persisted for the same reason as K_NEXT: a
// reboot must not hand back the line the pet just finished saying.
static const char*    K_LAST   = "cvlast";

void ConversationSystem::loadMemory()
{
    if (save_->loadU32(K_VER, 0) != MEM_VERSION) {   // explicit format gate
        memset(facts_, 0, sizeof facts_);
        memset(seen_,  0, sizeof seen_);
        memset(jrn_,   0, sizeof jrn_);
        ESP_LOGI(TAG, "no conversation memory yet (or a new format)");
        return;
    }

    if (save_->loadBlob(K_FACTS, facts_, sizeof facts_)) {
        uint32_t n = save_->loadU32(K_FACTN, 0);
        factCount_ = (int)(n > CONV_MAX_FACTS ? CONV_MAX_FACTS : n);
    } else {
        memset(facts_, 0, sizeof facts_);
    }

    if (save_->loadBlob(K_SEEN, seen_, sizeof seen_)) {
        uint32_t h = save_->loadU32(K_SEENH, 0);
        uint32_t n = h >> 16;
        seenCount_ = (int)(n > CONV_SEEN_MAX ? CONV_SEEN_MAX : n);
        seenHead_  = (int)((h & 0xFFFFu) % CONV_SEEN_MAX);
    } else {
        memset(seen_, 0, sizeof seen_);
    }

    if (save_->loadBlob(K_JRN, jrn_, sizeof jrn_)) {
        uint32_t h = save_->loadU32(K_JRNH, 0);
        uint32_t n = h >> 16;
        jrnCount_ = (int)(n > CONV_JOURNAL_MAX ? CONV_JOURNAL_MAX : n);
        jrnHead_  = (int)((h & 0xFFFFu) % CONV_JOURNAL_MAX);
    } else {
        memset(jrn_, 0, sizeof jrn_);
    }
    ESP_LOGI(TAG, "memory: %d facts, %d seen, %d journal", factCount_, seenCount_, jrnCount_);
    // Facts are what make later conversations reference earlier answers, so list them: a gate
    // that silently never matches is otherwise indistinguishable from "no content applies".
    for (int i = 0; i < factCount_; i++)
        ESP_LOGI(TAG, "  fact: %s = %s", facts_[i].key, facts_[i].val);
}

void ConversationSystem::saveMemory()
{
    // Written straight from the member arrays -- see the note above loadMemory().
    // One batched commit: these seven writes used to pay seven open/commit/close cycles.
    save_->beginBatch();
    save_->storeBlob(K_FACTS, facts_, sizeof facts_);
    save_->storeU32(K_FACTN, (uint32_t)factCount_);

    save_->storeBlob(K_SEEN, seen_, sizeof seen_);
    save_->storeU32(K_SEENH, ((uint32_t)seenCount_ << 16) | (uint32_t)seenHead_);

    save_->storeBlob(K_JRN, jrn_, sizeof jrn_);
    save_->storeU32(K_JRNH, ((uint32_t)jrnCount_ << 16) | (uint32_t)jrnHead_);

    save_->storeU32(K_VER, MEM_VERSION);   // last inside the batch: committed atomically
    save_->endBatch();
    memDirty_ = false;
}

// RAM-side mutations (markSeen/recordJournal/setFact) only set memDirty_; this is the one
// flush point. finish()/dismiss() call it, so memory hits NVS once per conversation instead
// of once per mutation (ending one conversation used to rewrite all ~6.5 KB twice or more).
void ConversationSystem::flushMemory()
{
    if (memDirty_) saveMemory();
}

// Ring, newest-first. jrnHead_ is the next write slot, so index 0 is the slot behind it; the
// modulo makes the same expression correct whether or not the ring has wrapped.
const ConvJournalEntry& ConversationSystem::journalAt(int i) const
{
    int slot = (jrnHead_ - 1 - i + 2 * CONV_JOURNAL_MAX) % CONV_JOURNAL_MAX;
    return jrn_[slot];
}

void ConversationSystem::recordJournal(const char* id, const char* title)
{
    ConvJournalEntry& e = jrn_[jrnHead_];
    memset(&e, 0, sizeof e);
    strncpy(e.id, id, CONV_ID_MAX - 1);
    strncpy(e.title, (title && title[0]) ? title : id, CONV_TITLE_MAX - 1);
    e.when = clock_now();
    jrnHead_ = (jrnHead_ + 1) % CONV_JOURNAL_MAX;
    if (jrnCount_ < CONV_JOURNAL_MAX) jrnCount_++;      // oldest silently falls off once full
}

void ConversationSystem::setFact(const char* key, const char* val, const char* note)
{
    if (!key || !*key) return;
    dirty_ = true;      // fact gates may have just opened or closed
    for (int i = 0; i < factCount_; i++)
        if (strcmp(facts_[i].key, key) == 0) {
            strncpy(facts_[i].val, val ? val : "", CONV_KEY_MAX - 1);
            facts_[i].val[CONV_KEY_MAX - 1] = '\0';
            if (note && note[0]) {                  // a re-answer replaces the journal phrasing
                strncpy(facts_[i].note, note, CONV_NOTE_MAX - 1);
                facts_[i].note[CONV_NOTE_MAX - 1] = '\0';
            }
            memDirty_ = true;                       // flushed by finish()/dismiss()
            return;
        }
    if (factCount_ >= CONV_MAX_FACTS) {
        ESP_LOGW(TAG, "fact table full; '%s' dropped", key);
        return;
    }
    strncpy(facts_[factCount_].key, key, CONV_KEY_MAX - 1);
    facts_[factCount_].key[CONV_KEY_MAX - 1] = '\0';
    strncpy(facts_[factCount_].val, val ? val : "", CONV_KEY_MAX - 1);
    facts_[factCount_].val[CONV_KEY_MAX - 1] = '\0';
    strncpy(facts_[factCount_].note, note ? note : "", CONV_NOTE_MAX - 1);
    facts_[factCount_].note[CONV_NOTE_MAX - 1] = '\0';
    factCount_++;
    ESP_LOGI(TAG, "fact set: %s = %s", key, val ? val : "");
    memDirty_ = true;                               // flushed by finish()/dismiss()
}

const char* ConversationSystem::fact(const char* key) const
{
    if (!key) return nullptr;
    for (int i = 0; i < factCount_; i++)
        if (strcmp(facts_[i].key, key) == 0) return facts_[i].val;
    return nullptr;
}

// Seen-history is stored as 32-bit hashes in a ring: bounded RAM and NVS regardless of how
// much the player gets through. A hash collision produces a rare, harmless "already seen".
bool ConversationSystem::isSeen(const char* id) const
{
    const uint32_t h = fnv1a(id);
    for (int i = 0; i < seenCount_; i++) if (seen_[i] == h) return true;
    return false;
}

void ConversationSystem::markSeen(const char* id)
{
    if (isSeen(id)) return;
    const uint32_t h = fnv1a(id);
    if (seenCount_ < CONV_SEEN_MAX) {
        seen_[seenCount_++] = h;
    } else {
        seen_[seenHead_] = h;                       // oldest falls off
        seenHead_ = (seenHead_ + 1) % CONV_SEEN_MAX;
    }
    memDirty_ = true;                               // flushed by finish()/dismiss()
}

void ConversationSystem::clearSeen()
{
    seenCount_ = 0;
    seenHead_  = 0;
    memset(seen_, 0, sizeof seen_);
    jrnCount_ = 0;                 // the journal is this creature's diary, so it goes too
    jrnHead_  = 0;
    memset(jrn_, 0, sizeof jrn_);
    dirty_ = true;                 // everything one-shot is eligible again
    lastPlayed_ = 0;               // a new creature is not gagged by its predecessor's last line
    saveMemory();
    save_->storeU32(K_LAST, 0);
    ESP_LOGI(TAG, "history + journal cleared (new creature); facts kept (they're about the PLAYER)");
}

// --- init ------------------------------------------------------------------------

void ConversationSystem::init(SaveStore& save)
{
    save_ = &save;

    // PSRAM: see the note on act_-> Aborting matches gfx_init()'s handling of a failed
    // framebuffer -- without it every later access is a null dereference.
    act_ = (Conversation*)heap_caps_malloc(sizeof(Conversation), MALLOC_CAP_SPIRAM);
    if (!act_) {
        ESP_LOGE(TAG, "PSRAM alloc for the active conversation (%u bytes) failed",
                 (unsigned)sizeof(Conversation));
        abort();
    }
    memset(act_, 0, sizeof(Conversation));
    ESP_LOGI(TAG, "active-conversation buffer: %u bytes in PSRAM (%d nodes max)",
             (unsigned)sizeof(Conversation), CONV_MAX_NODES);

    gamedata_mount();
    loadMemory();

    lastPlayed_ = save_->loadU32(K_LAST, 0);   // no repeat across a reboot either

    // Resume the persisted cooldown so a reboot can't be used to skip the gap between
    // conversations (repeatable ones would otherwise be farmable by power-cycling).
    uint32_t next = save_->loadU32(K_NEXT, 0);
    uint32_t now  = clock_now();
    cooldown_ = (next > now) ? (float)(next - now) : 0.0f;
    // Clamped, because K_NEXT was written against whatever the clock read back then: a clock
    // since set BACKWARDS (or one that was reset while the pet kept playing) would otherwise
    // resume a cooldown of years and lock conversations out for good.
    if (cooldown_ > COOLDOWN_MAX) cooldown_ = COOLDOWN_MAX;
}

// --- gate evaluation -------------------------------------------------------------

bool ConversationSystem::gatePasses(void* json, const ConvContext& ctx, bool* outUnseen,
                                    int16_t* outPriority, const char* id) const
{
    cJSON* root = (cJSON*)json;
    *outPriority = (int16_t)gd_num(root, "priority", 0);

    // Triggered conversations live OUTSIDE the ambient pool: the entry's "trigger" must
    // equal what the current scan wants ("" for the ambient scan), or a deathbed farewell
    // would fire as small talk -- and small talk would fire at a deathbed.
    {
        char trig[16];
        gd_str(root, "trigger", trig, sizeof trig, "");
        if (strcmp(trig, wantTrigger_) != 0) return false;
    }

    const bool repeatable = gd_bool(root, "repeatable", false);
    const bool seen       = isSeen(id);
    *outUnseen = !seen;
    if (seen && !repeatable) return false;

    cJSON* w = cJSON_GetObjectItem(root, "when");
    if (!w) return true;                            // no gate = always eligible

    if (ctx.friendship < jgate(w, "minFriendship", 0, 0, 65535)) return false;
    if (ctx.friendship > jgate(w, "maxFriendship", 65535, 0, 65535)) return false;
    if (ctx.stage      < jgate(w, "minStage", 0, 0, 255)) return false;
    if (ctx.wins       < jgate(w, "minWins", 0, 0, 4294967295.0)) return false;
    // Miracles granted so far: lets a second farewell acknowledge the first ("you're
    // sitting with me again") instead of replaying itself verbatim.
    if (ctx.reprieves  < jgate(w, "minReprieves", 0, 0, 255)) return false;
    if (ctx.reprieves  > jgate(w, "maxReprieves", 255, 0, 255)) return false;

    if (!str_gate(w, "nature",  ctx.nature))  return false;
    if (!str_gate(w, "personality", ctx.trait)) return false;
    if (!str_gate(w, "species", ctx.species)) return false;
    // Life-track gate ("prime"/"elderly"/"twilight" -- docs/death-and-lifespan.md §4): how
    // aged conversation content is served. An unknown token in a modded file simply never
    // matches, which is the loud no-op every other string gate gives a typo.
    if (!str_gate(w, "life", ctx.life)) return false;

    // Mood gate. "upset" matches hurt OR angry, so one resolution conversation can cover both
    // without being written twice.
    cJSON* mg = cJSON_GetObjectItem(w, "mood");
    if (cJSON_IsString(mg) && mg->valuestring && mg->valuestring[0]) {
        const bool upset = ctx.mood && strcmp(ctx.mood, "ok") != 0;
        if (strcmp(mg->valuestring, "upset") == 0) { if (!upset) return false; }
        else if (!ctx.mood || strcmp(mg->valuestring, ctx.mood) != 0) return false;
    }

    cJSON* v = cJSON_GetObjectItem(w, "sick");
    if (cJSON_IsBool(v) && cJSON_IsTrue(v) != ctx.sick) return false;
    v = cJSON_GetObjectItem(w, "hungry");
    if (cJSON_IsBool(v) && cJSON_IsTrue(v) != ctx.hungry) return false;

    // hour window, wrapping at midnight
    cJSON* h0 = cJSON_GetObjectItem(w, "hourMin");
    cJSON* h1 = cJSON_GetObjectItem(w, "hourMax");
    if (cJSON_IsNumber(h0) && cJSON_IsNumber(h1)) {
        int a = (int)h0->valuedouble, b = (int)h1->valuedouble;
        bool in = (a <= b) ? (ctx.hour >= a && ctx.hour <= b)
                           : (ctx.hour >= a || ctx.hour <= b);
        if (!in) return false;
    }

    // Chaining: this conversation only unlocks once another has been seen.
    cJSON* rs = cJSON_GetObjectItem(w, "requireSeen");
    if (cJSON_IsString(rs) && rs->valuestring && rs->valuestring[0] && !isSeen(rs->valuestring))
        return false;

    // Fact gates: { "fact": { "key": "value" } }, and "" matches "set to anything".
    cJSON* fg = cJSON_GetObjectItem(w, "fact");
    if (cJSON_IsObject(fg)) {
        cJSON* it = nullptr;
        cJSON_ArrayForEach(it, fg) {
            const char* have = fact(it->string);
            if (!have) return false;
            if (cJSON_IsString(it) && it->valuestring && it->valuestring[0] &&
                strcmp(have, it->valuestring) != 0) return false;
        }
    }
    cJSON* nf = cJSON_GetObjectItem(w, "notFact");
    if (cJSON_IsString(nf) && nf->valuestring && hasFact(nf->valuestring)) return false;

    return true;
}

// --- reservoir -------------------------------------------------------------------

// Rank order: more specific pool first, then higher priority, then unseen one-shots ahead of
// repeatable filler. ONE definition, used by both reservoir eviction and final selection --
// if the two ever weighted differently, eviction would quietly disagree with the pick.
long ConversationSystem::cand_rank(const Cand& x)
{
    return (long)x.pool * 1000000L + (long)x.priority * 10L + (x.unseen ? 1L : 0L);
}

// Only the top RESERVOIR candidates are ever held, which is what keeps selection RAM
// independent of how many conversations are installed.
void ConversationSystem::offer(const Cand& c)
{
    // Same id already held: the later-scanned copy wins. SD roots are walked after flash
    // (and more specific pools after generic ones), so this is what makes an SD conversation
    // OVERRIDE a base one with the same id -- the modding contract every other registry
    // implements -- instead of both entering the draw and markSeen() retiring the pair.
    for (int i = 0; i < resCount_; i++)
        if (strcmp(res_[i].id, c.id) == 0) { res_[i] = c; return; }

    if (resCount_ < RESERVOIR) { res_[resCount_++] = c; return; }

    int worst = 0;
    for (int i = 1; i < resCount_; i++)
        if (cand_rank(res_[i]) < cand_rank(res_[worst])) worst = i;
    if (cand_rank(c) > cand_rank(res_[worst])) res_[worst] = c;
}

void ConversationSystem::choose()
{
    if (resCount_ <= 0) return;

    // Triggered searches take the best outright: farewell tiers are authored to outrank
    // each other (bonded over trusted, a repeat visit over both), and a deathbed is not
    // the moment for the anti-repetition roll below.
    if (pickBest_) {
        int best = 0;
        for (int i = 1; i < resCount_; i++)
            if (cand_rank(res_[i]) > cand_rank(res_[best])) best = i;
        if (loadFile(res_[best].path, res_[best].entry)) {
            pending_ = true;
            ESP_LOGI(TAG, "triggered '%s' chosen from %d candidates", act_->id, resCount_);
        }
        return;
    }

    // Weighted-random among the survivors, favouring the better-ranked ones. Keeps the pet
    // from opening with the same line every time.
    for (int i = 0; i < resCount_ - 1; i++)             // small n: simple selection sort
        for (int j = i + 1; j < resCount_; j++)
            if (cand_rank(res_[j]) > cand_rank(res_[i])) { Cand t = res_[i]; res_[i] = res_[j]; res_[j] = t; }

    int total = 0;
    for (int i = 0; i < resCount_; i++) total += (resCount_ - i);
    int r = (int)(esp_random() % (uint32_t)total);
    int pick = resCount_ - 1;
    for (int i = 0; i < resCount_; i++) {
        r -= (resCount_ - i);
        if (r < 0) { pick = i; break; }
    }

    if (loadFile(res_[pick].path, res_[pick].entry)) {
        pending_ = true;
        ESP_LOGI(TAG, "offering '%s' (pool %u, %d nodes) from %d candidates",
                 act_->id, res_[pick].pool, act_->nodeCount, resCount_);
    }
}

// --- body load (only the CHOSEN conversation is ever parsed into RAM) ------------

bool ConversationSystem::loadFile(const char* path, int entry)
{
    char* buf = gd_read_file(path, MAX_FILE_BYTES);
    if (!buf) return false;
    cJSON* file = cJSON_Parse(buf);
    free(buf);
    if (!file) { ESP_LOGW(TAG, "bad json: %s", path); return false; }

    // One conversation, or the requested element of a pack.
    cJSON* root = file;
    if (cJSON_IsArray(file)) {
        root = cJSON_GetArrayItem(file, entry);
        if (!root) {
            ESP_LOGW(TAG, "%s has no entry %d", path, entry);
            cJSON_Delete(file);
            return false;
        }
    }

    memset(act_, 0, sizeof(Conversation));
    gd_str(root, "id", act_->id, sizeof act_->id, "");
    gd_str(root, "title", act_->title, sizeof act_->title, act_->id);   // journal label
    // Kept on the loaded conversation so the scene can tell Pet whether a friendship
    // effect is a one-shot milestone or metered routine chatter (see applyConversationChoice).
    act_->repeatable = gd_bool(root, "repeatable", false);
    char startId[16];
    gd_str(root, "start", startId, sizeof startId, "");
    // The fate fork's entry points, read HERE while the document is still alive -- the
    // whole cJSON tree is freed the moment the nodes are marshalled, and the substitution
    // pass below runs after that, on act_ alone.
    char onDeath[16], onReprieve[16];
    gd_str(root, "onDeath",    onDeath,    sizeof onDeath,    "");
    gd_str(root, "onReprieve", onReprieve, sizeof onReprieve, "");

    cJSON* nodes = cJSON_GetObjectItem(root, "nodes");
    // NB: always delete `file`, never `root` -- with a pack, `root` is an element INSIDE it.
    if (!cJSON_IsArray(nodes)) { cJSON_Delete(file); return false; }

    cJSON* n = nullptr;
    cJSON_ArrayForEach(n, nodes) {
        if (act_->nodeCount >= CONV_MAX_NODES) {
            ESP_LOGW(TAG, "'%s' exceeds %d nodes; extra ignored", act_->id, CONV_MAX_NODES);
            break;
        }
        ConvNode& nd = act_->nodes[act_->nodeCount];
        memset(&nd, 0, sizeof nd);
        gd_str(n, "id",   nd.id,   sizeof nd.id,   "");
        gd_str(n, "text", nd.text, sizeof nd.text, "");
        gd_str(n, "to",   nd.to,   sizeof nd.to,   "");   // choiceless continuation
        nd.toIdx = -1;

        cJSON* chs = cJSON_GetObjectItem(n, "choices");
        if (cJSON_IsArray(chs)) {
            cJSON* c = nullptr;
            cJSON_ArrayForEach(c, chs) {
                if (nd.choiceCount >= CONV_MAX_CHOICES) break;
                ConvChoice& ch = nd.choices[nd.choiceCount];
                memset(&ch, 0, sizeof ch);
                gd_str(c, "text", ch.text, sizeof ch.text, "...");
                gd_str(c, "to",   ch.to,   sizeof ch.to,   "end");
                ch.toIdx = -1;

                cJSON* fx = cJSON_GetObjectItem(c, "effects");
                // Clamped, not cast: an out-of-range modded value must not wrap through int16.
                double fr = gd_num(fx, "friendship", 0);
                double hp = gd_num(fx, "happiness",  0);
                ch.fx.friendship = (int16_t)(fr > 32767 ? 32767 : (fr < -32768 ? -32768 : fr));
                ch.fx.happiness  = (int16_t)(hp > 32767 ? 32767 : (hp < -32768 ? -32768 : hp));
                parse_drift(fx, "drift", ch.fx.drift, 0.0f);
                cJSON* sf = fx ? cJSON_GetObjectItem(fx, "setFact") : nullptr;
                if (cJSON_IsObject(sf) && sf->child) {
                    strncpy(ch.fx.factKey, sf->child->string ? sf->child->string : "",
                            CONV_KEY_MAX - 1);
                    if (cJSON_IsString(sf->child) && sf->child->valuestring)
                        strncpy(ch.fx.factVal, sf->child->valuestring, CONV_KEY_MAX - 1);
                }
                if (fx) {
                    gd_str(fx, "factNote", ch.fx.factNote, CONV_NOTE_MAX, "");
                    gd_str(fx, "setMood",  ch.fx.setMood,  sizeof ch.fx.setMood, "");
                }
                nd.choiceCount++;
            }
        }
        act_->nodeCount++;
    }
    cJSON_Delete(file);                              // freed at once: cJSON never holds two docs

    // The fate fork (docs/death-and-lifespan.md §6): "@fate" jump targets resolve NOW, at
    // load, to the entry point matching the roll that was persisted before this
    // conversation was even searched for. The runtime never sees "@fate" -- so a dialogue
    // choice structurally CANNOT steer the outcome, only witness it: fate has exactly one
    // door, and it was walked through at the brink.
    act_->fateIdx = -1;
    {
        const char* authored = (loadFate_ == FATE_MIRACLE) ? onReprieve : onDeath;
        const char* target   = authored[0] ? authored : "end";   // missing half: end, loudly
        bool warned = false, swapped = false;
        for (int i = 0; i < act_->nodeCount; i++) {
            ConvNode& nd = act_->nodes[i];
            if (strcmp(nd.to, "@fate") == 0) {
                if (!authored[0] && !warned) { warned = true;
                    ESP_LOGW(TAG, "'%s': @fate with no matching on%s", act_->id,
                             loadFate_ == FATE_MIRACLE ? "Reprieve" : "Death"); }
                strncpy(nd.to, target, sizeof nd.to - 1); nd.to[sizeof nd.to - 1] = '\0';
                swapped = true;
            }
            for (int j = 0; j < nd.choiceCount; j++)
                if (strcmp(nd.choices[j].to, "@fate") == 0) {
                    strncpy(nd.choices[j].to, target, sizeof nd.choices[j].to - 1);
                    nd.choices[j].to[sizeof nd.choices[j].to - 1] = '\0';
                    swapped = true;
                }
        }
        // Remember WHERE fate lands so the scene can stage the moment (see Conversation).
        if (swapped && authored[0])
            for (int k = 0; k < act_->nodeCount; k++)
                if (strcmp(act_->nodes[k].id, target) == 0) { act_->fateIdx = (int8_t)k; break; }
    }

    // resolve choice + node continuation targets to node indices
    for (int i = 0; i < act_->nodeCount; i++) {
        for (int j = 0; j < act_->nodes[i].choiceCount; j++) {
            ConvChoice& ch = act_->nodes[i].choices[j];
            ch.toIdx = -1;
            for (int k = 0; k < act_->nodeCount; k++)
                if (strcmp(act_->nodes[k].id, ch.to) == 0) { ch.toIdx = (int8_t)k; break; }
        }
        if (act_->nodes[i].to[0])
            for (int k = 0; k < act_->nodeCount; k++)
                if (strcmp(act_->nodes[k].id, act_->nodes[i].to) == 0) {
                    act_->nodes[i].toIdx = (int8_t)k;
                    break;
                }
    }

    act_->startIdx = 0;
    if (startId[0])
        for (int k = 0; k < act_->nodeCount; k++)
            if (strcmp(act_->nodes[k].id, startId) == 0) { act_->startIdx = (int8_t)k; break; }

    return act_->nodeCount > 0;
}

// --- streaming scan --------------------------------------------------------------

// The stage floor is a content rule expressed as a number, so keep it pinned to the enum it
// means rather than to the literal 2 it happens to be.
static_assert(CONV_MIN_STAGE == STAGE_IN_TRAINING_2,
              "CONV_MIN_STAGE must stay the first stage that may talk");

void ConversationSystem::beginScan()
{
    scanning_  = true;
    scanPool_  = 0;
    scanRoot_  = 0;
    resCount_  = 0;
    scanFiles_ = 0;
    if (dir_) { closedir((DIR*)dir_); dir_ = nullptr; }
    scanStart_ = esp_timer_get_time();
}

// Throw away a half-finished scan. The reservoir goes too: choose() must never be reached with
// candidates gathered for a creature that is no longer allowed to speak.
void ConversationSystem::abortScan()
{
    if (dir_) { closedir((DIR*)dir_); dir_ = nullptr; }
    scanning_ = false;
    resCount_ = 0;
}

// Walks (pool x root) pairs, opening the next directory that exists. Returns false when the
// whole library has been covered.
bool ConversationSystem::openNextDir()
{
    // Roots per pool: 0 = flash, 1..pakfs_count() = mounted mod packs, last = loose SD.
    const int nRoots = 2 + pakfs_count();
    while (scanPool_ < POOL_COUNT) {
        const bool inPak  = scanRoot_ > 0 && scanRoot_ < nRoots - 1;
        const char* pakRt = inPak ? pakfs_root(scanRoot_ - 1) : nullptr;

        // Species conversations ride along in the creature's own folder rather than a pool
        // directory, so they need the creature id rather than a fixed name.
        if (scanPool_ == POOL_SPECIES) {
            if (!species_[0]) { scanPool_++; scanRoot_ = 0; continue; }
            if (scanRoot_ == 0)
                snprintf(dirPath_, sizeof dirPath_, "/creatures/%s/conversations", species_);
            else if (inPak)
                snprintf(dirPath_, sizeof dirPath_, "%s/creatures/%s/conversations", pakRt, species_);
            else
                snprintf(dirPath_, sizeof dirPath_, "/sdcard/creatures/%s/conversations", species_);
        } else {
            const char* pd = POOL_DIR[scanPool_];
            if (scanRoot_ == 0) snprintf(dirPath_, sizeof dirPath_, "%s/conversations/%s", GAMEDATA_ROOT, pd);
            else if (inPak)     snprintf(dirPath_, sizeof dirPath_, "%s/conversations/%s", pakRt, pd);
            else                snprintf(dirPath_, sizeof dirPath_, "/sdcard/conversations/%s", pd);
        }

        curPool_ = (uint8_t)scanPool_;                        // remember BEFORE the cursor moves
        if (++scanRoot_ >= nRoots) { scanRoot_ = 0; scanPool_++; }

        DIR* d = opendir(dirPath_);
        if (d) { dir_ = d; return true; }
    }
    return false;
}

void ConversationSystem::stepScan(const ConvContext& ctx, int budget)
{
    while (budget > 0) {
        if (!dir_ && !openNextDir()) {               // library fully covered
            scanning_ = false;
            scanMs_ = (uint32_t)((esp_timer_get_time() - scanStart_) / 1000);
            // Reported so the need for the derived index (doc 5.2) can be judged from
            // measurement rather than guessed at.
            ESP_LOGI(TAG, "scan: %d files, %u ms, %d eligible", scanFiles_,
                     (unsigned)scanMs_, resCount_);
            choose();
            return;
        }

        struct dirent* ent = readdir((DIR*)dir_);
        if (!ent) { closedir((DIR*)dir_); dir_ = nullptr; continue; }
        if (ent->d_name[0] == '.') continue;

        char path[128];
        snprintf(path, sizeof path, "%s/%s", dirPath_, ent->d_name);

        // Read + parse ONE file, gate everything in it, keep at most a reservoir slot, free it.
        // This is what makes RAM independent of library size.
        char* buf = gd_read_file(path, MAX_FILE_BYTES);
        budget--;                       // charged per FILE: the open is the expensive part, and
        scanFiles_++;                   // gating a pack's entries afterwards costs no I/O
        if (!buf) continue;
        cJSON* file = cJSON_Parse(buf);
        free(buf);
        if (!file) { ESP_LOGW(TAG, "bad json: %s", path); continue; }

        // A pack (JSON array) holds many conversations; a bare object holds one. Walking the
        // array's children directly avoids re-indexing it for every entry.
        const bool isPack = cJSON_IsArray(file);
        cJSON* item = isPack ? file->child : file;
        for (int entry = 0; item; entry++, item = isPack ? item->next : nullptr)
            considerEntry(item, path, entry, ctx);

        cJSON_Delete(file);
    }
}

void ConversationSystem::considerEntry(void* itemPtr, const char* path, int entry,
                                       const ConvContext& ctx)
{
    cJSON* item = (cJSON*)itemPtr;

    char id[CONV_ID_MAX];
    gd_str(item, "id", id, sizeof id, "");
    if (!id[0]) return;                 // unusable without an id (it keys seen/journal)

    // Never say the same thing twice running. Ambient eligibility is often narrow -- the hour
    // window on a late-night line can leave it as the ONLY candidate, and the weighted roll in
    // choose() then re-picks it every cooldown until the hour rolls over. Excluded HERE rather
    // than at the bubble so a different conversation gets the slot when one exists; when the
    // repeat really is the only thing eligible, the pet just stays quiet.
    //
    // Triggered searches are exempt: a deathbed farewell is authored to be the right line for
    // the moment, and staying silent there because it was also the last thing said is worse
    // than the repeat.
    if (!pickBest_ && lastPlayed_ && fnv1a(id) == lastPlayed_) {
        ESP_LOGD(TAG, "skipping '%s': just played", id);
        return;
    }

    bool unseen = false;
    int16_t prio = 0;
    if (!gatePasses(item, ctx, &unseen, &prio, id)) return;

    Cand c{};
    strncpy(c.id, id, sizeof c.id - 1);
    strncpy(c.path, path, sizeof c.path - 1);
    c.entry    = (int16_t)entry;
    c.priority = prio;
    c.pool     = curPool_;              // latched when the directory was opened
    c.unseen   = unseen;
    offer(c);
}

void ConversationSystem::update(float dt, const ConvContext& ctx, bool allowScan)
{
    if (cooldown_ > 0.0f) cooldown_ -= dt;

    // Remembered so the scan can find the creature's own conversation folder; it changes on
    // evolution, which is exactly when a new form's dialogue should become available.
    strncpy(species_, ctx.species ? ctx.species : "", sizeof species_ - 1);
    species_[sizeof species_ - 1] = '\0';

    // Tracked FIRST and unconditionally, so that whatever changed while conversations were
    // suppressed (a nap, hunger, hatching past the stage floor) is already recognised as a
    // trigger the moment they're allowed again.
    const uint32_t h = ctx_hash(ctx);
    if (h != ctxHash_) { ctxHash_ = h; dirty_ = true; }

    // Too young to talk (CONV_MIN_STAGE). Placed above everything else because the stage can
    // also DROP mid-session -- a new egg, or the devolve cheat -- which must tear down work
    // already in flight rather than leave a bubble on an egg or let a scan finish and offer.
    // The stage is part of the fingerprint above, so evolving past the floor is itself the
    // trigger for the first scan.
    if (ctx.stage < CONV_MIN_STAGE) {
        if (scanning_) abortScan();
        pending_ = false;
        return;
    }

    // allowScan=false pauses the FAT+parse work entirely: a scan file costs ~7-12 ms, which
    // is a missed parry window in battle or a hitch mid-minigame. An in-flight scan just
    // holds its directory handle and resumes when the caller is back somewhere uncritical.
    if (scanning_) { if (allowScan) stepScan(ctx, SCAN_BUDGET); return; }

    lastFriendship_ = ctx.friendship;    // finish()/dismiss() need it to roll the next gap

    if (pending_ || ctx.asleep || !allowScan) return;   // never interrupt, nag, or scan mid-play

    sinceScan_ += dt;
    if (cooldown_ > 0.0f || sinceScan_ < MIN_SCAN_GAP) return;
    if (dirty_ || sinceScan_ >= FALLBACK_SECS) {
        dirty_     = false;
        sinceScan_ = 0.0f;
        beginScan();
    }
}

bool ConversationSystem::triggeredStep(const char* trigger, const ConvContext& ctx, int budget)
{
    if (strcmp(wantTrigger_, trigger) != 0) {
        // First call: take the scan machinery over. Ambient work in flight -- including a
        // pending offer waiting as a bubble somewhere -- is discarded: whatever the pet
        // wanted to chat about no longer applies at a deathbed.
        abortScan();
        pending_ = false;
        strncpy(wantTrigger_, trigger, sizeof wantTrigger_ - 1);
        wantTrigger_[sizeof wantTrigger_ - 1] = '\0';
        pickBest_ = true;
        loadFate_ = ctx.fate;             // "@fate" resolves against this at load time
        beginScan();
    }
    if (scanning_) stepScan(ctx, budget);
    if (scanning_) return false;          // more files to walk: call again next frame

    wantTrigger_[0] = '\0';               // done (choose() already ran, argmax + load)
    pickBest_ = false;
    dirty_    = true;                     // ambient state was torn up; rescan when it may
    return true;
}

void ConversationSystem::dismiss()
{
    // A declined ONE-SHOT is deliberately re-offered (see dirty_ below) -- you may simply not
    // have been ready to answer it. Declined CHATTER is not: re-offering the line you just
    // backed out of is the same spam finish() guards against, one dismiss at a time.
    if (act_->id[0] && act_->repeatable) lastPlayed_ = fnv1a(act_->id);

    pending_  = false;
    cooldown_ = roll_cooldown(lastFriendship_);
    persistCooldown();   // flushes any facts set before the player bailed out, too
    dirty_    = true;    // declined, not resolved: re-offer once the cooldown lapses
}

// Flush pending memory and stamp the RTC second the next offer may roll.
void ConversationSystem::persistCooldown()
{
    flushMemory();
    save_->beginBatch();                 // one commit for both
    save_->storeU32(K_NEXT, clock_now() + (uint32_t)cooldown_);
    save_->storeU32(K_LAST, lastPlayed_);
    save_->endBatch();
}

void ConversationSystem::finish()
{
    if (act_->id[0]) {
        const bool firstTime = !isSeen(act_->id);
        lastPlayed_ = fnv1a(act_->id);       // the next ambient scan will not offer it again
        markSeen(act_->id);                  // gating
        // Journal only the FIRST completion: a repeatable conversation replayed a few times
        // a day would otherwise fill the 32-slot ring with duplicates and silently evict
        // the one-shot milestone memories the journal exists to commemorate.
        if (firstTime) recordJournal(act_->id, act_->title);
        memDirty_ = true;
    }
    pending_  = false;
    cooldown_ = roll_cooldown(lastFriendship_);
    persistCooldown();                       // one memory flush + the next-offer time
    // Our own memory just changed: this one is now excluded and a `requireSeen` chain may have
    // unlocked. The context fingerprint can't see that, so trigger a rescan explicitly.
    dirty_    = true;
    ESP_LOGI(TAG, "finished '%s'; next conversation in ~%.0f min", act_->id, cooldown_ / 60.0f);
}
