#pragma once
#include "engine/input.hpp"
#include "engine/power.hpp"
#include "sim/save.hpp"
#include "sim/creatures.hpp"
#include "sim/pet.hpp"
#include "scene.hpp"
#include "scenes/care/scene_home.hpp"
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
#include "scenes/menus/scene_cheats.hpp"
#include "scenes/menus/scene_timeset.hpp"
#include "scenes/menus/scene_stats.hpp"
#include "scenes/menus/scene_rename.hpp"

enum class SceneId { Home, Menu, Activities, Run, MindMaze, Smash, Bulwark, Stance, Battle, BattleSelect, Settings, Cheats, TimeSet, Stats, Rename };

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
    Pet              pet{save, creatures};
    bool             debugOverlay = false;   // on-screen debug info (persisted via save)

    SceneManager  scenes;
    SceneHome     home;
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
    SceneCheats   cheats;
    SceneTimeSet  timeset;
    SceneStats    stats;
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
