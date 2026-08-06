#pragma once
#include "engine/input.hpp"
#include "engine/power.hpp"
#include "sim/save.hpp"
#include "sim/creatures.hpp"
#include "sim/foods.hpp"
#include "sim/personality.hpp"
#include "sim/conversation.hpp"
#include "sim/pet.hpp"
#include "scene.hpp"
#include "scenes/care/scene_home.hpp"
#include "scenes/care/scene_feed.hpp"
#include "scenes/care/scene_conversation.hpp"
#include "scenes/minigames/scene_run.hpp"
#include "scenes/minigames/scene_mindmaze.hpp"
#include "scenes/minigames/scene_smash.hpp"
#include "scenes/minigames/scene_bulwark.hpp"
#include "scenes/minigames/scene_stance.hpp"
#include "scenes/battle/scene_battle.hpp"
#include "scenes/menus/scene_battle_select.hpp"
#include "scenes/menus/scene_activities.hpp"
#include "scenes/menus/scene_menu.hpp"
#include "scenes/menus/scene_settings.hpp"
#include "scenes/menus/scene_update.hpp"
#include "scenes/menus/scene_cheats.hpp"
#include "scenes/menus/scene_timeset.hpp"
#include "scenes/menus/scene_stats.hpp"
#include "scenes/menus/scene_journal.hpp"
#include "scenes/menus/scene_rename.hpp"

enum class SceneId { Home, Feed, Conversation, Menu, Activities, Run, MindMaze, Smash, Bulwark, Stance, Battle, BattleSelect, Settings, Update, Cheats, TimeSet, Stats, Journal, Rename };

// Scene-change animation. Forward = going deeper (new covers, slides in from the
// right with overshoot); Back = returning (old slides off, revealing new);
// Iris = cartoon circle close/open (used for the minigame); None = instant.
enum class Slide { None, Forward, Back, Iris };

// Composition root: owns the subsystems and scenes, runs the game loop, and is
// passed to every scene so they don't reach for globals.
class App {
public:
    SaveStore        save;
    InputManager     input;
    CreatureRegistry creatures;          // data-driven roster (flash + SD), loaded in init()
    FoodRegistry     foods;              // data-driven food list (gamedata + SD), loaded in init()
    PersonalityRegistry personalities;   // data-driven natures + traits (gamedata + SD)
    PersonalityTracker  drift{save, personalities};   // emergent identity; fed by Pet's actions
    ConversationSystem  conversations;    // moddable dialogue: streaming selection, O(1) RAM
    Pet              pet{save, creatures};
    bool             debugOverlay = false;   // on-screen debug info (persisted via save)

    SceneManager  scenes;
    SceneHome     home;
    SceneFeed     feedScene;             // food picker (named to avoid reading like pet.feed)
    SceneConversation conversationScene;
    SceneMenu     menu;
    SceneActivities activities;
    SceneRun      run;
    SceneMindMaze mindmaze;
    SceneSmash    smash;
    SceneBulwark  bulwark;
    SceneStance   stance;
    SceneBattle   battle;
    SceneBattleSelect battleSelect;
    SceneSettings settings;
    SceneUpdate   updateScene;           // SD-card firmware install (named to avoid Scene::update)
    SceneCheats   cheats;
    SceneTimeSet  timeset;
    SceneStats    stats;
    SceneJournal  journal;
    SceneRename   rename;

    void init();               // bind scenes, boot pet, load flags, enter Home
    void runLoop();            // the forever render/sim loop (never returns)
    void setScene(SceneId id, Slide slide = Slide::None);   // switch scene, optionally animated

private:
    bool  transitioning_ = false;   // a slide is in progress (input suppressed)
    float transT_ = 0.0f;           // 0..1 slide progress
    Slide slide_ = Slide::None;

    PowerManager power_;   // device sleep: mode + button gestures + auto-sleep
};
