#pragma once
#include "core/scene.hpp"
#include "sim/conversation.hpp"   // CONV_MAX_CHOICES (per-node layout cache)

// Talking to the creature: the payoff a high bond exists for. Reached by tapping the speech
// bubble on Home when ConversationSystem has one waiting.
//
// Text types itself out (gfx_text_wrap's `reveal` parameter, added in Phase 0 for exactly
// this), then the choices appear. Tapping mid-type skips the animation rather than being
// ignored -- rereading a line you've already seen shouldn't cost you a wait.
//
// A "Later" button dismisses the conversation (it re-offers after a normal cooldown), and
// the scene deliberately does NOT block device sleep: with no exit and sleep disabled, an
// abandoned dialogue kept the backlight and CPU on until the battery died.
//
// DEATHBED MODE (returnTo(SceneId::Death), set by SceneDeath before entering): the same
// scene carries the farewell conversation. Three things differ -- every exit returns to
// the death event instead of Home, the "Later" button is absent (a farewell cannot be
// postponed; the tree's ends are the only way through), and the farewell music keeps
// playing (the conversation is part of the death event, not a break from it).
class SceneConversation : public Scene {
    int   node_    = 0;       // index into the active conversation's node list
    float reveal_  = 0.0f;    // characters revealed so far (fractional)
    float t_       = 0.0f;    // for the blinking continue caret
    SceneId ret_   = SceneId::Home;   // where every exit goes; reset to Home on exit
    // The fate moment (deathbed only). Entering the fate node first CARRIES the creature
    // to the centre of the screen, then holds a silent beat -- no dialogue, no input --
    // before the verdict's first word; a miracle breaks the silence with a white flash and
    // a creature that is visibly GLAD. The verdict dialogue plays beneath the centred
    // creature, and a death hands over to SceneDeath with it already in place to sleep.
    float slideT_   = 0.0f;   // centre-ward drift remaining
    float silenceT_ = 0.0f;   // seconds of held silence remaining
    float flashT_   = 0.0f;   // white-flash decay remaining (miracle only)
    bool  centered_ = false;  // the creature has crossed; portrait + panel use centre layout
    bool  happy_    = false;  // portrait shows the happy frame (fate landed on a miracle)

    bool deathbed() const { return ret_ == SceneId::Death; }
    void leave();                            // setScene(ret_) with the right slide/sound
    void drawFlash();                        // the miracle's white flash, over everything

    // Per-node layout cache, rebuilt on node entry. Wrapped-text measurement is a full
    // greedy pass over the string; recomputing it per frame (and per hit-test) for text
    // that only changes on node transitions was pure waste.
    int  speechH_ = 0;                       // wrapped height of the speech text
    int  textLen_ = 0;                       // strlen of the speech text (typewriter end)
    int  chH_[CONV_MAX_CHOICES]     = {0};   // full button height per choice
    int  chTextH_[CONV_MAX_CHOICES] = {0};   // wrapped label height (for vertical centering)

    void enterNode(int idx);                 // set node_ + rebuild the layout cache

public:
    void onEnter() override;
    void onExit() override { ret_ = SceneId::Home; }   // one-shot: never leaks past a visit
    void update(float dt) override;
    void render() override;
    void onInput(const Input& in) override;

    void returnTo(SceneId id) { ret_ = id; }   // call BEFORE setScene(Conversation)
    const char* musicId() const override
    {
        return deathbed() ? sfx::kMusicFarewell : sfx::kMusicHome;
    }
};
