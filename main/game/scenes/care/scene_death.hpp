#pragma once
#include "core/scene.hpp"

// The death event (docs/death-and-lifespan.md §7): one scene for both flavours of the
// brink and both verdicts of the roll. The sequence ALWAYS begins -- the collapse comes
// first, the verdict after -- so even a survivor's player has witnessed the stakes. Fate
// itself was rolled and persisted the moment the brink was reached (Pet::enterBrink);
// nothing done in here can change it, only reveal it.
//
// Deliberately spare: no chrome, no Back, the first seconds accept no input at all. The
// farewell music (sfx::kMusicFarewell) plays here and nowhere else in the game.
class SceneDeath : public Scene {
    enum class Ph : uint8_t {
        Dusk,       // the light has gone down around them; the creature stands, quiet
        Collapse,   // it sinks
        Still,      // held breath
        Verdict,    // the miracle interrupts -- or the farewell is said (tap to continue)
        Passing,    // after a death farewell: it drifts to the centre, sleeps, and the
                    //   frame darkens to black (gfx_fade)
        Memorial,   // who they were; "Begin again" passes the torch
        Depart,     // the long goodbye: memorial and music fade out together, then the
                    //   chip restarts into the next generation
    };
    Ph    ph_   = Ph::Dusk;
    float t_    = 0.0f;      // seconds in the current phase
    bool  saidGoodbye_ = false;   // creature's voice played once, at the collapse
    // Deathbed farewell (D3, old-age brinks only): the search for an eligible conversation
    // runs a few files per frame UNDER the prologue, whose ~7 seconds hide even a large
    // modded library. If one is found, the Still phase hands over to SceneConversation
    // instead of showing the verdict text; it returns here via resumeFromFarewell().
    bool  searching_ = false;
    bool  haveConv_  = false;
    bool  afterConv_ = false;
    bool  fadeInMem_ = false;   // Memorial was reached through the dark: ramp the light back

    void toPhase(Ph p) { ph_ = p; t_ = 0.0f; }
public:
    // Called by SceneConversation's exit path (via App) before it returns to this scene:
    // the farewell has been said and fate revealed in dialogue -- skip the prologue and
    // the verdict text, and resolve straight to Home (miracle) or the memorial (death).
    void resumeFromFarewell() { afterConv_ = true; }
    void onEnter() override;
    void update(float dt) override;
    void render() override;
    void onInput(const Input& in) override;

    // The world is stopped and stays stopped: no napping mid-farewell, and the care sim
    // does not advance beneath the event (the brink gate already suspends the pet; the
    // careSpeed override covers everything else the loop scales by it).
    bool  allowsSleep() const override { return false; }
    float careSpeed()  const override { return 0.0f; }
    const char* musicId() const override { return sfx::kMusicFarewell; }
};
