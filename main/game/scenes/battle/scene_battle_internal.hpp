#pragma once
// Shared layout + tuning constants for the battle scene, used by both the logic
// (scene_battle.cpp) and rendering (scene_battle_draw.cpp) translation units.

// --- layout ---
static const int ENEMY_CX = 120, ENEMY_CY = 94;
static const int PLAYER_CX = 120, PLAYER_CY = 192;
static const int SPRITE_BOX = 90;
static const int BTN_Y = 280, BTN_H = 36, BTN_W = 108;
static const int STRIKE_BX = 8, SPECIAL_BX = 124;

// --- animation tuning ---
static const float BOB_AMP = 3.0f,  BOB_SPD = 2.2f;
static const float LUNGE_DUR = 0.28f, LUNGE_AMP = 18.0f;
static const float FLASH_DUR = 0.35f, KB_AMP = 8.0f;
static const float FAINT_DUR = 0.9f;
static const float SHAKE_DECAY = 3.2f, SHAKE_AMP = 6.0f, SHAKE_AMP_BIG = 12.0f;
static const float HP_FRONT_EASE = 16.0f;   // front HP bar catches the real value quickly
static const float GHOST_DELAY   = 0.55f;    // how long the phantom holds before draining
static const float GHOST_EASE    = 3.5f;     // phantom's slow catch-down rate
static const float JUDGE_DUR = 0.75f;
static const float SHOCK_DUR = 0.45f;

static const float RING_RMAX = 60.0f, RING_RTARGET = 20.0f, RING_RMIN = 10.0f, RING_WINDOW = 26.0f;
static const float RING_DUR_ATK = 0.95f, RING_DUR_PARRY = 0.72f, RING_DUR_COMBO = 0.62f;
