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
class SceneConversation : public Scene {
    int   node_    = 0;       // index into the active conversation's node list
    float reveal_  = 0.0f;    // characters revealed so far (fractional)
    float t_       = 0.0f;    // for the blinking continue caret

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
    void update(float dt) override;
    void render() override;
    void onInput(const Input& in) override;
};
