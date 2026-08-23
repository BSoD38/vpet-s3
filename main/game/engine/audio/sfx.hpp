#pragma once
#include "audio.hpp"

// The sound events the game itself fires, as ids rather than an enum.
//
// Ids and not an enum ON PURPOSE: the bank is data (bank.hpp), so a mod pack can define new
// sounds and reference them from its own data files -- a creature with its own cry, a food
// with its own crunch. An enum would make the engine's set closed and the mod's set second
// class. These constants exist so the C++ side has one spelling of each id and a typo is a
// link error instead of a sound that quietly never plays.
//
// Every id here must exist in flash_gamedata/sounds/sounds.json. A missing id is not a crash
// -- play() returns kNoHandle -- but it is a bug, because it means an event has no voice.

namespace sfx {

// --- UI ------------------------------------------------------------------------------------
inline constexpr const char* kTap      = "ui_tap";       // any button / list row
inline constexpr const char* kBack     = "ui_back";      // leaving a screen
inline constexpr const char* kSelect   = "ui_select";    // committing a choice
inline constexpr const char* kDenied   = "ui_denied";    // action refused (frozen, no energy)

// --- care ------------------------------------------------------------------------------
inline constexpr const char* kFeed     = "feed";         // food offered
inline constexpr const char* kEat      = "eat";          // each bite
inline constexpr const char* kRefuse   = "refuse";       // pet turns the food down
inline constexpr const char* kClean    = "clean";        // poop cleared
inline constexpr const char* kHeal     = "heal";         // medicine
inline constexpr const char* kPet      = "pet_happy";    // a happy reaction
inline constexpr const char* kSad      = "pet_sad";      // an unhappy one
inline constexpr const char* kSick     = "pet_sick";
inline constexpr const char* kSleep    = "sleep";        // lights off
inline constexpr const char* kWake     = "wake";         // lights on

// --- milestones ----------------------------------------------------------------------
inline constexpr const char* kHatch    = "hatch";
inline constexpr const char* kEvolve   = "evolve";
inline constexpr const char* kLevelUp  = "levelup";

// --- battle ------------------------------------------------------------------------------
inline constexpr const char* kHit      = "hit";
inline constexpr const char* kCrit     = "crit";
inline constexpr const char* kParry    = "parry";
inline constexpr const char* kMiss     = "miss";
inline constexpr const char* kWin      = "win";
inline constexpr const char* kLose     = "lose";

// --- minigames -----------------------------------------------------------------------
inline constexpr const char* kScore    = "score";
inline constexpr const char* kCountIn  = "countin";
inline constexpr const char* kGameOver = "gameover";

// --- music -----------------------------------------------------------------------------
inline constexpr const char* kMusicHome   = "bgm_home";
inline constexpr const char* kMusicBattle = "bgm_battle";
// The farewell. Played by SceneDeath's musicId() and NOWHERE else -- reuse would spend it.
// The one time the player hears this, it should only ever have meant one thing.
inline constexpr const char* kMusicFarewell = "bgm_farewell";

// Fire-and-forget shorthand. Reads better at a call site that does not care about the
// handle, which is nearly all of them.
inline void play(const char* id) { audio::play(id); }
inline void play(const char* id, float gain) { audio::play(id, gain); }

}   // namespace sfx
