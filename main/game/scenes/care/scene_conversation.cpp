#include "scene_conversation.hpp"
#include "core/app.hpp"
#include "engine/gfx.hpp"
#include "ui/widgets.hpp"
#include "sim/creatures.hpp"
#include "assets/sprites.hpp"
#include "esp_log.h"
#include <cstring>

static const Sprite SPR_FALLBACK { spr_unknown_data, SPRITE_W, SPRITE_H, SPRITE_TRANSP };

// layout. The top band is kept tight so the reply buttons below can afford two lines each.
static const int PORTRAIT_CY = 56;                  // creature, centred in the top band
static const int PORTRAIT_BOX = 76;
static const int BUBBLE_X = 12, BUBBLE_Y = 100, BUBBLE_W = GAME_W - 24;
static const int TEXT_PAD = 10;
static const int CHOICE_GAP = 6;
static const int CHOICE_BOT = GAME_H - 10;          // bottom-most choice ends here
static const int CHOICE_PAD = 8;                    // inside a reply button

// Both blocks are line-capped so a long modded string can't grow its box into the one below.
static const int SPEECH_LINES = 5;
static const int CHOICE_LINES = 2;

static const float CHARS_PER_SEC = 42.0f;           // typewriter speed

// "Not now": dismisses the conversation (it re-offers after a normal cooldown). Top-right,
// clear of the centred portrait. Without an exit, the only way out was finishing the tree.
static const Rect LATER{ GAME_W - 78, 12, 66, 30 };

static int choice_text_w() { return GAME_W - 24 - 2 * CHOICE_PAD; }

// Rebuild the per-node layout cache: wrapped heights + text length only change when the
// node does, so they're measured once here rather than every frame and every hit-test.
void SceneConversation::enterNode(int idx)
{
    node_   = idx;
    reveal_ = 0.0f;

    const Conversation& c = app().conversations.active();
    if (node_ < 0 || node_ >= c.nodeCount) { speechH_ = textLen_ = 0; return; }
    const ConvNode& nd = c.nodes[node_];

    speechH_ = gfx_text_wrap_height(BUBBLE_W - 2 * TEXT_PAD, 1, nd.text, 4, SPEECH_LINES);
    textLen_ = (int)strlen(nd.text);

    for (int i = 0; i < nd.choiceCount; i++) {
        // A reply is as tall as its own wrapped text needs, with a floor that keeps it
        // comfortably tappable. Measured with the same helper and cap the renderer uses,
        // so layout and drawing can't desync.
        int th = gfx_text_wrap_height(choice_text_w(), 1, nd.choices[i].text, 3, CHOICE_LINES);
        chTextH_[i] = th;
        int h = th + 2 * CHOICE_PAD;
        chH_[i] = h < 30 ? 30 : h;                  // touch-target floor
    }
}

// Bottom-aligned: the last reply always sits in the same place, so muscle memory holds no
// matter how many options a node has. Uses the cached heights.
static Rect choice_rect(const int* chH, int choiceCount, int i)
{
    int total = 0;
    for (int k = 0; k < choiceCount; k++) total += chH[k];
    total += (choiceCount - 1) * CHOICE_GAP;

    int y = CHOICE_BOT - total;
    for (int k = 0; k < i; k++) y += chH[k] + CHOICE_GAP;
    return { 12, y, GAME_W - 24, chH[i] };
}

void SceneConversation::onEnter()
{
    t_ = 0.0f;
    enterNode(app().conversations.active().startIdx);
}

void SceneConversation::update(float dt)
{
    t_ += dt;
    reveal_ += CHARS_PER_SEC * dt;
}

void SceneConversation::render()
{
    const Conversation& c = app().conversations.active();
    Pet& pet = app().pet;

    fb.fillScreen(col::panel);

    // Who's talking. Feet-anchored and scaled down to fit, like every other screen that shows
    // the creature at a size it wasn't drawn for.
    LGFX_Sprite* spr = app().creatures.sprite(pet.creatureIndex());
    if (spr) gfx_blit_sprite_fit(spr, GAME_W / 2, PORTRAIT_CY, PORTRAIT_BOX, PORTRAIT_BOX, SPRITE_TRANSP);
    else     gfx_blit(SPR_FALLBACK, GAME_W / 2, PORTRAIT_CY);
    gfx_text(12, 12, 1, col::accent, "%s", pet.displayName());

    LATER.button("Later", rgb565(56, 60, 74), col::dim, 1);

    if (node_ < 0 || node_ >= c.nodeCount) return;
    const ConvNode& nd = c.nodes[node_];

    // Speech panel, sized to the wrapped text so short lines don't leave a big empty box.
    const int textW = BUBBLE_W - 2 * TEXT_PAD;
    const int panelH = speechH_ + 2 * TEXT_PAD;
    Rect panel{ BUBBLE_X, BUBBLE_Y, BUBBLE_W, panelH };
    panel.fill(col::card, 8);
    panel.outline(col::dim, 8);
    // Little tail pointing up at the creature, so the panel reads as speech.
    fb.fillTriangle(GAME_W / 2 - 7, BUBBLE_Y, GAME_W / 2 + 7, BUBBLE_Y,
                    GAME_W / 2, BUBBLE_Y - 9, col::card);

    const int shown = (int)reveal_;
    gfx_text_wrap(BUBBLE_X + TEXT_PAD, BUBBLE_Y + TEXT_PAD, textW, 1, col::white,
                  nd.text, 4, shown, SPEECH_LINES);

    const bool typing = shown < textLen_;

    if (typing) {
        gfx_text(GAME_W - 74, GAME_H - 24, 1, col::dim, "tap to skip");
        return;
    }

    if (nd.choiceCount == 0) {
        // A statement the creature is making: blink a caret rather than show a dead button.
        if ((int)(t_ * 2.0f) % 2 == 0)
            gfx_text(GAME_W / 2 - 18, CHOICE_BOT - 18, 2, col::accent, "...");
        return;
    }

    // Rect::button() centres ONE size-2 line, which silently overflows past ~18 characters --
    // far too short for a real reply. So the geometry still comes from Rect (fill/outline/
    // contains stay in one place) while the label is drawn wrapped at size 1.
    for (int i = 0; i < nd.choiceCount; i++) {
        Rect r = choice_rect(chH_, nd.choiceCount, i);
        r.fill(col::accent, 8);
        r.outline(rgb565(120, 90, 20), 8);
        gfx_text_wrap(r.x + CHOICE_PAD, r.y + (r.h - chTextH_[i]) / 2, choice_text_w(), 1,
                      col::black, nd.choices[i].text, 3, -1, CHOICE_LINES);
    }
}

void SceneConversation::onInput(const Input& in)
{
    if (!in.pressed) return;

    // Leaving mid-conversation is always allowed: dismiss() re-offers it after a normal
    // cooldown, and any facts already set by earlier choices are flushed there.
    if (LATER.contains(in)) {
        app().conversations.dismiss();
        app().setScene(SceneId::Home, Slide::Back);
        return;
    }

    const Conversation& c = app().conversations.active();
    if (node_ < 0 || node_ >= c.nodeCount) { app().setScene(SceneId::Home, Slide::Back); return; }
    const ConvNode& nd = c.nodes[node_];

    // Mid-type: any tap completes the line instead of being swallowed.
    if ((int)reveal_ < textLen_) {
        reveal_ = (float)textLen_;
        return;
    }

    if (nd.choiceCount == 0) {
        if (nd.toIdx >= 0) {                       // the creature has more to say
            enterNode(nd.toIdx);
            return;
        }
        app().conversations.finish();               // last word spoken: mark seen + journal it
        app().setScene(SceneId::Home, Slide::Back);
        return;
    }

    for (int i = 0; i < nd.choiceCount; i++) {
        if (!choice_rect(chH_, nd.choiceCount, i).contains(in)) continue;
        const ConvChoice& ch = nd.choices[i];

        // Effects go through Pet (the funnel for bond/happiness/drift) and the conversation
        // system (which owns the fact store). Fact and mood land BEFORE
        // applyConversationChoice so its markSaved() persists them in the same batch.
        if (ch.fx.factKey[0])
            app().conversations.setFact(ch.fx.factKey, ch.fx.factVal, ch.fx.factNote);
        if (ch.fx.setMood[0]) {
            PetMood m;
            // Unknown tokens are a loud no-op: mapping them to "ok" would silently mend a
            // rift the writer had just opened (the old behavior for any typo'd mod value).
            if (mood_from_id(ch.fx.setMood, &m)) app().pet.setMood(m);
            else ESP_LOGW("CONV", "unknown setMood '%s' ignored", ch.fx.setMood);
        }
        // Repeatable conversations are metered routine chatter; one-shots are milestones.
        app().pet.applyConversationChoice(ch.fx.friendship, ch.fx.happiness, ch.fx.drift,
                                          c.repeatable ? BOND_ROUTINE : BOND_MILESTONE);

        if (ch.toIdx >= 0) {                        // continue to the next node
            enterNode(ch.toIdx);
            return;
        }
        app().conversations.finish();                // "end": mark seen, start the cooldown
        app().setScene(SceneId::Home, Slide::Back);
        return;
    }
}
