#pragma once
#include "engine/input.hpp"
#include "engine/power.hpp"
#include "sim/save.hpp"
#include "sim/creatures.hpp"
#include "sim/foods.hpp"
#include "sim/items.hpp"
#include "sim/economy.hpp"
#include "sim/room.hpp"
#include "sim/personality.hpp"
#include "sim/conversation.hpp"
#include "sim/pet.hpp"
#include "scene.hpp"
#include "scenes/care/scene_home.hpp"
#include "scenes/care/scene_feed.hpp"
#include "scenes/care/scene_medicine.hpp"
#include "scenes/care/scene_conversation.hpp"
#include "scenes/care/scene_death.hpp"
#include "scenes/minigames/scene_run.hpp"
#include "scenes/minigames/scene_mindmaze.hpp"
#include "scenes/minigames/scene_smash.hpp"
#include "scenes/minigames/scene_bulwark.hpp"
#include "scenes/minigames/scene_stance.hpp"
#include "scenes/minigames/scene_work.hpp"
#include "scenes/battle/scene_battle.hpp"
#include "scenes/menus/scene_battle_select.hpp"
#include "scenes/menus/scene_activities.hpp"
#include "scenes/menus/scene_menu.hpp"
#include "scenes/menus/scene_settings.hpp"
#include "scenes/menus/scene_update.hpp"
#include "scenes/menus/scene_cheats.hpp"
#include "scenes/menus/scene_timeset.hpp"
#include "scenes/menus/scene_about.hpp"
#include "scenes/menus/scene_stats.hpp"
#include "scenes/menus/scene_journal.hpp"
#include "scenes/menus/scene_rename.hpp"
#include "scenes/menus/scene_shop.hpp"

// SceneId and Slide live in core/scene.hpp (scene headers need them; this header includes
// those before it could define anything).

// Composition root: owns the subsystems and scenes, runs the game loop, and is
// passed to every scene so they don't reach for globals.
class App {
public:
    SaveStore        save;
    InputManager     input;
    CreatureRegistry creatures;          // data-driven roster (flash + SD), loaded in init()
    FoodRegistry     foods;              // data-driven food list (gamedata + SD), loaded in init()
    ItemRegistry     items;              // data-driven item list (toys/decor/medicine), loaded in init()
    Economy          economy;            // Bits + the bag; own NVS keys, survives death
    Room             room;               // what is PUT OUT (toy now, decor at E5); survives death
    PersonalityRegistry personalities;   // data-driven natures + traits (gamedata + SD)
    PersonalityTracker  drift{save, personalities};   // emergent identity; fed by Pet's actions
    ConversationSystem  conversations;    // moddable dialogue: streaming selection, O(1) RAM
    Pet              pet{save, creatures};
    bool             debugOverlay = false;   // on-screen debug info (persisted via save)

    SceneManager  scenes;
    SceneHome     home;
    SceneFeed     feedScene;             // food picker (named to avoid reading like pet.feed)
    SceneMedicine medicine;              // treatment picker (Heal is an item now, not a free tap)
    SceneConversation conversationScene;
    SceneDeath    deathScene;            // the death event (entered by the brink, never by menu)
    SceneMenu     menu;
    SceneActivities activities;
    SceneRun      run;
    SceneMindMaze mindmaze;
    SceneSmash    smash;
    SceneBulwark  bulwark;
    SceneStance   stance;
    SceneWork     work;                  // pet-less sorting shift: the floor under priced treatment
    SceneBattle   battle;
    SceneBattleSelect battleSelect;
    SceneSettings settings;
    SceneUpdate   updateScene;           // SD-card firmware install (named to avoid Scene::update)
    SceneCheats   cheats;
    SceneTimeSet  timeset;
    SceneAbout    about;                 // firmware / hardware / power readout
    SceneStats    stats;
    SceneJournal  journal;
    SceneRename   rename;
    SceneShop     shop;                  // Shop + Bag tabs; never gated (it is the player's screen)

    void init();               // bind scenes, boot pet, load flags, enter Home
    void runLoop();            // the forever render/sim loop (never returns)
    void setScene(SceneId id, Slide slide = Slide::None);   // switch scene, optionally animated
    // The player has seen the memorial: conclude the death (generation++, save invalidated)
    // and restart the chip. The fresh boot IS the fresh start -- hatching, per-creature
    // resets and the conversation-history clear all run through the one tested path.
    [[noreturn]] void restartAfterDeath();
    // The gate context the run loop feeds the conversation system, for scenes that drive
    // their own conversation search (SceneDeath's deathbed farewell).
    ConvContext convCtx();

    // Point Pet at the out toy's drift vector (or clear it). Called after anything that can
    // change which toy is out, and once at boot. Lives here because App is the only thing
    // that can see the Room and the ItemRegistry at the same time -- Pet knows neither.
    void refreshAmbientToy();

private:
    bool  transitioning_ = false;   // a slide is in progress (input suppressed)
    float transT_ = 0.0f;           // 0..1 slide progress
    Slide slide_ = Slide::None;

    PowerManager power_;   // device sleep: mode + button gestures + auto-sleep
};
