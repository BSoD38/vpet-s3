#pragma once
#include <cstdint>
#include "personality.hpp"   // AX_COUNT (choice effects can nudge drift)

class SaveStore;

// Moddable conversation trees the creature initiates -- the payoff for a high bond, and the
// thing that gives personality an output. Full design in docs/conversations-and-personality.md.
//
// THE HARD CONSTRAINT: RAM is O(1) in the number of installed conversations. A player with 20
// and a player with 20,000 use the same memory. That is achieved by never holding the library:
//   * selection STREAMS candidates and keeps only the best few in a fixed reservoir (5.1),
//   * only the CHOSEN conversation is parsed into a fixed struct, and the cJSON doc is freed
//     immediately (5.3),
//   * play-growing state (facts, seen-history) is explicitly capped (5.6).
// A derived per-pool index (doc 5.2) is a pure I/O optimisation and is deliberately NOT built
// yet -- the streaming scan is correct on its own, so speed gets measured before it's added.
//
// PACKS: a file may hold ONE conversation (a JSON object) or MANY (a JSON array of them). Both
// work in either root, so a mod can ship a single drop-in file or one pack containing a whole
// campaign. Packs matter for more than tidiness: the measured scan cost is dominated by file
// OPENS rather than parsing, and FAT allocates a ~4 KB cluster per file however small it is, so
// a pack of 30 costs one open instead of thirty and wastes one cluster instead of thirty.

// --- caps. Exceeding one is logged and the conversation skipped, never a crash. ---
constexpr int CONV_ID_MAX      = 40;
// A 3-wide, 2-deep tree with a distinct reaction per choice is already 13 nodes, so 12 capped
// conversations at "one question deep". 32 leaves room for genuinely branching, searching
// conversations at the high bond tiers. Raising this is CHEAP because the one resident
// conversation lives in PSRAM (see act_), not in scarce internal RAM.
constexpr int CONV_MAX_NODES   = 32;
constexpr int CONV_MAX_CHOICES = 3;
constexpr int CONV_TEXT_MAX    = 128;
// Replies get real room: a choice is something the PLAYER says, so one-word answers make the
// dialogue shallow. 96 chars is ~3 wrapped lines; the scene renders up to 2 and elides, so an
// author has space for a proper sentence without being able to break the layout.
constexpr int CONV_CHOICE_MAX  = 96;
constexpr int CONV_KEY_MAX     = 24;
constexpr int CONV_TITLE_MAX   = 32;   // journal entry label
constexpr int CONV_NOTE_MAX    = 48;   // human phrasing of a fact, for the journal

// Pools, in increasing specificity. A more specific conversation outranks a generic one when
// both are eligible.
enum ConvPool : uint8_t {
    POOL_NATURE = 0,      // "a Gentle creature would say..."
    POOL_TRAIT,           // "...specifically a Shy one"
    POOL_PLAYER,          // about YOU: hopes, likes, dreams
    POOL_SPECIES,         // ships with the creature
    POOL_COUNT
};

// What a choice does when taken.
struct ConvEffects {
    int16_t friendship;
    int16_t happiness;
    float   drift[AX_COUNT];
    char    factKey[CONV_KEY_MAX];    // "" = sets nothing
    char    factVal[CONV_KEY_MAX];
    // How the journal phrases this fact ("You like doing very little"). The key/value pair
    // stays machine-readable for gates; this is the version a human reads. Without it the
    // journal would show slugs, which is not much of an emotional payoff.
    char    factNote[CONV_NOTE_MAX];
    // "hurt" / "angry" / "ok": a choice can genuinely upset the creature, which is felt in the
    // care loop (it won't be petted) until a follow-up conversation mends it. "" = no change.
    char    setMood[8];
};

struct ConvChoice {
    char        text[CONV_CHOICE_MAX];
    char        to[16];               // target node id ("end"/"" ends the conversation)
    int8_t      toIdx;                // resolved after load; -1 = end
    ConvEffects fx;
};

struct ConvNode {
    char       id[16];
    char       text[CONV_TEXT_MAX];
    ConvChoice choices[CONV_MAX_CHOICES];
    uint8_t    choiceCount;           // 0 = a statement the player just taps through
    // Where a CHOICELESS node goes next ("" / "end" finishes). Lets the creature answer in
    // more than one beat without a meaningless "..." button between them, which is what
    // makes a reply feel like a reaction rather than a terminated exchange.
    char       to[16];
    int8_t     toIdx;
};

// The ONE conversation resident at a time.
struct Conversation {
    char     id[CONV_ID_MAX];
    char     title[CONV_TITLE_MAX];   // journal label; defaults to the id
    // Repeatable conversations are metered chatter, not milestones: the scene passes this
    // to Pet::applyConversationChoice so their friendship effects go through the daily
    // routine allowance instead of bypassing it (an unmetered repeatable was a bond grind).
    bool     repeatable;
    ConvNode nodes[CONV_MAX_NODES];
    uint8_t  nodeCount;
    int8_t   startIdx;
};

// One remembered conversation. Keeps the canonical id AND a cached title: the design intent
// was to re-render titles from the data files, but a file read costs ~10 ms on this hardware,
// so a journal page would stall to redraw. The cache is bounded by the ring, so it can't grow.
struct ConvJournalEntry {
    char     id[CONV_ID_MAX];
    char     title[CONV_TITLE_MAX];
    uint32_t when;                    // RTC seconds when it was finished
};

// Everything a gate is tested against, so the system stays decoupled from Pet.
struct ConvContext {
    uint16_t    friendship;
    uint32_t    wins;
    uint8_t     stage;
    const char* nature;      // "" when Unformed
    const char* trait;
    const char* species;
    bool        sick;
    bool        hungry;
    bool        asleep;
    int         hour;        // 0..23 sim hour
    const char* mood;        // "ok" / "hurt" / "angry"; gates may also ask for "upset"
};

// --- bounded memory (doc 5.6) --------------------------------------------------------------
// Facts are GLOBAL -- they outlive a creature, so a fresh one can still say "I hear you like
// exploring". Seen-history is per-creature and cleared with a new egg.
constexpr int CONV_MAX_FACTS   = 32;
constexpr int CONV_SEEN_MAX    = 256;
constexpr int CONV_JOURNAL_MAX = 32;   // remembered conversations, newest-first (a knob)

struct ConvFact {
    char key[CONV_KEY_MAX];
    char val[CONV_KEY_MAX];
    char note[CONV_NOTE_MAX];          // journal phrasing; falls back to "key: value"
};

class ConversationSystem {
public:
    void init(SaveStore& save);

    // Time-sliced: advances a streaming scan one file per call, so a large library never
    // hitches a frame. Safe (and cheap) to call every frame. Pass allowScan=false in
    // timing-critical scenes (minigame/battle): cooldown and context tracking still run,
    // but no FAT I/O or parsing happens until the caller allows it again.
    void update(float dt, const ConvContext& ctx, bool allowScan = true);

    bool                pending() const { return pending_; }
    const Conversation& active()  const { return *act_; }
    void                dismiss();                 // player closed it without finishing
    void                finish();                  // ran to the end: mark seen + start cooldown

    // Facts: the memory of the player's answers, and the mechanism behind conversations that
    // depend on earlier choices.
    void        setFact(const char* key, const char* val, const char* note = nullptr);
    const char* fact(const char* key) const;       // nullptr if unset
    bool        hasFact(const char* key) const { return fact(key) != nullptr; }

    void clearSeen();                              // new egg: forget this creature's history
    int  factCount() const { return factCount_; }
    const ConvFact& factAt(int i) const { return facts_[i]; }
    int  seenCount() const { return seenCount_; }

    // Journal: remembered conversations, index 0 = most recent.
    int  journalCount() const { return jrnCount_; }
    const ConvJournalEntry& journalAt(int i) const;

    // Dev/diagnostic: how long the last full library scan took, and how many files it read.
    uint32_t lastScanMs()    const { return scanMs_; }
    int      lastScanFiles() const { return scanFiles_; }

private:
    // One reservoir slot. Fixed count, so RAM is independent of library size.
    static const int RESERVOIR = 8;
    struct Cand {
        char    id[CONV_ID_MAX];
        char    path[128];
        int16_t entry;        // index within a pack file; 0 for a single-conversation file
        int16_t priority;
        uint8_t pool;
        bool    unseen;
    };

    SaveStore* save_ = nullptr;

    // The single resident conversation, in PSRAM: ~28 KB at 32 nodes, which would be a lot of
    // internal RAM for something read a handful of times per frame. Keeping it here means the
    // node cap is a content decision rather than a memory one. Allocated once in init().
    Conversation* act_ = nullptr;
    bool         pending_  = false;
    float        cooldown_ = 0.0f;

    // streaming scan state
    bool     scanning_   = false;
    int      scanPool_   = 0;
    int      scanRoot_   = 0;      // 0 = flash, 1..pakfs_count() = mod packs, last = SD
    uint8_t  curPool_    = 0;      // pool the OPEN directory belongs to (scanPool_ has moved on)
    char     species_[24]{};       // current creature id, for its own conversation folder
    void*    dir_        = nullptr;
    char     dirPath_[128]{};
    Cand     res_[RESERVOIR];
    int      resCount_   = 0;
    int      scanFiles_  = 0;
    int64_t  scanStart_  = 0;
    uint32_t scanMs_     = 0;
    float    sinceScan_  = 0.0f;
    // Change-triggered scanning: `ctxHash_` fingerprints everything a gate can test, and
    // `dirty_` is set when it moves or when our own memory changes (a conversation finished, a
    // fact was set). Starts dirty so the first scan happens shortly after boot.
    uint32_t ctxHash_    = 0;
    bool     dirty_      = true;
    uint16_t lastFriendship_ = 0;   // latched from ctx; the next gap is rolled against it

    ConvFact facts_[CONV_MAX_FACTS];
    int      factCount_ = 0;
    uint32_t seen_[CONV_SEEN_MAX];
    int      seenCount_ = 0;
    int      seenHead_  = 0;
    ConvJournalEntry jrn_[CONV_JOURNAL_MAX];
    int      jrnCount_ = 0;        // saturates at CONV_JOURNAL_MAX
    int      jrnHead_  = 0;        // next write slot (ring)

    void beginScan();
    void stepScan(const ConvContext& ctx, int budget);
    bool openNextDir();
    static long cand_rank(const Cand& c);   // ONE ranking for eviction and selection
    void offer(const Cand& c);
    void choose();
    bool loadFile(const char* path, int entry);
    // Gate one conversation (a whole file, or one element of a pack) and offer it to the
    // reservoir. `item` is a cJSON* -- kept as void* so the header needn't include cJSON.
    void considerEntry(void* item, const char* path, int entry, const ConvContext& ctx);
    bool gatePasses(void* json, const ConvContext& ctx, bool* outUnseen,
                    int16_t* outPriority, const char* id) const;
    bool isSeen(const char* id) const;
    void markSeen(const char* id);
    void recordJournal(const char* id, const char* title);
    void loadMemory();
    void saveMemory();
    void flushMemory();       // write memory to NVS iff a RAM mutation is pending
    void persistCooldown();   // flushMemory + stamp the next-offer RTC second

    bool memDirty_ = false;   // facts/seen/journal changed in RAM since the last save
};
