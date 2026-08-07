# Sleep & Power

Two power-saving modes on top of the normal Active game loop. This is **device** sleep —
distinct from the pet's own day/night `lightsOff` (its scheduled nap, which dims the panel
but keeps the game running). The pet can be napping while the device is Active, and the
device can be asleep while the pet's schedule says "awake".

A third, unrelated kind of "away" lives at the bottom of this doc: the
[care freeze](#care-freeze--the-pet-being-away-not-the-device), which stops the *simulation*
rather than the hardware.

Code: [`engine/power.hpp`](../main/game/engine/power.hpp) /
[`engine/power.cpp`](../main/game/engine/power.cpp), driven from
[`core/app.cpp`](../main/game/core/app.cpp) `runLoop()` and routed at boot by
[`main.cpp`](../main/main.cpp).

## Why not the ULP coprocessor

The original idea was to offload background processing to the ULP. We didn't, on purpose:

- **Timekeeping is already external.** The PCF85063 is a *battery-backed* RTC. It keeps
  wall-clock time whether the ESP32 is awake, asleep, or off. On wake,
  [`clock.cpp`](../main/game/engine/clock.cpp) `clock_elapsed()` gives the delta for free.
- **Pet care across sleep is already solved.** [`Pet::boot()`](../main/game/sim/pet.cpp)
  runs an *offline catch-up*: it replays `tick()` over the elapsed real seconds (in 60 s
  steps, capped at 24 h). Deep sleep is just "offline", so the exact same code applies.
- **The ULP can't run the sim.** `tick()` is float-heavy (`expf`, decay rates) and evolution
  reads the creature registry loaded from flash/SD. The ULP-RISC-V has no FPU, ~8 KB of RTC
  RAM, and no access to that data — it would mean a hand-ported fixed-point *duplicate* of
  the whole simulation for negligible battery gain over the approach below.

Instead: **deep sleep with a periodic RTC-timer wake** (CPU wakes briefly every
`POWER_DEEP_POLL_S`, default 15 min) **+ instant GPIO wake on the PWR button**.

## Light sleep

Screen off, simulation still running. Entered by a short PWR press or after
`AUTO_LIGHT_US` (3 min) of no input; exited by any touch or PWR press. If nothing happens
for `AUTO_DEEP_US` (15 min total idle) it **escalates to deep sleep** on its own — light
sleep still runs the CPU at 240 MHz, so the real battery savings only start at deep sleep.
(Manual deep sleep is the ~1 s PWR hold below.)

### Where sleep is allowed

Sleeping (light/deep, auto *or* button) is gated per-scene by
[`Scene::allowsSleep()`](../main/game/core/scene.hpp) — default `true`, overridden to
`false` in the timed/active scenes (the minigames under
[`scenes/minigames/`](../main/game/scenes/minigames) and
[`SceneBattle`](../main/game/scenes/battle/scene_battle.hpp)) so a nap or an idle
timeout can't interrupt play. **Power-off (long hold) is exempt** — it works everywhere.
(The battle *select* menu and the activities picker still allow sleep; only the live
minigames/battle forbid it.)

- Backlight forced dark via `Backlight_Suspend(1)` and the ST7789 put to `SLPIN`
  (`display.sleep()`). `Backlight_Suspend` remembers the pet's last scheduled brightness so
  resume restores it (see [`ST7789.c`](../main/drivers/LCD_Driver/ST7789.c)).
- The game loop keeps calling `pet.tick()` and autosaving; it just skips scene input and
  rendering, and idles at 100 ms/iteration. The core-0 driver task (RTC / battery / IMU) is
  untouched.
- **CPU stays at 240 MHz** for now. Dropping to 80 MHz needs the PM framework
  (`CONFIG_PM_ENABLE` + `esp_pm_configure`) plus power-locks on SPI/LEDC/audio — deferred as
  a follow-up because it risks display/audio glitches until tuned. The big win (killing the
  render loop) is already captured.

## Deep sleep

Everything but the RTC domain powers down. The board **stays powered** because the power
latch (GPIO7) is held high through sleep (`gpio_hold_en` + `gpio_deep_sleep_hold_en`);
releasing it is how [`Shutdown()`](../main/drivers/PWR_Key/PWR_Key.c) actually powers off.

Wake sources armed on entry:
- **RTC timer** every `POWER_DEEP_POLL_S` — the background heartbeat.
- **PWR key** (GPIO6, active-low) via `esp_deep_sleep_enable_gpio_wakeup` — instant user wake.

A deep-sleep wake is a full chip reset, so the two boot paths are routed in `app_main` by
`esp_sleep_get_wakeup_cause()`:

- **Timer wake** → `power_service_timer_wake()`: bring up only NVS + I2C + RTC + creature
  registry (no display/audio/Wi-Fi), replay the offline catch-up, test the wake triggers.
  If none fired, persist and re-enter deep sleep — the display never turns on. If one fired,
  return and let `app_main` do a full boot.
- **Anything else** (PWR key, cold boot, reset) → straight to a full boot.

### Wake triggers

Checked after each timer-wake catch-up:

| Trigger        | Condition                          |
|----------------|------------------------------------|
| Evolution      | life stage increased during sleep  |
| Low hunger     | `hunger < 15`                      |
| Low happiness  | `happiness < 20`                   |
| Low HP         | `health < 20`                      |

Threshold triggers are **edge-detected** with hysteresis (re-arm above 25/30/30) so a stat
that simply sits low doesn't relight the screen every 15 min. The "already alerted" bitmask
is stored in its **own NVS key** (`slpAlert`), *not* in the `PetState` blob — so this feature
never bumps `PET_VERSION` or wipes an existing pet.

## Care freeze — the *pet* being away, not the device

**Settings → GAME → Freeze care.** Device sleep keeps the simulation running; the care freeze
stops it. It exists for the stretches where the player genuinely can't look after the creature
(a trip, a hospital stay, a week the device spends in a drawer) and would otherwise come back
to a starved, sick, aged pet they never had a chance to prevent.

The deal is symmetrical, and deliberately so: **nothing moves, and you can't touch it either.**
Aging, evolution, the day/night clock, hunger, happiness, health, energy, poop, sickness, the
neglect clock and personality drift all stop — and feeding, cleaning, healing, petting, poking,
the lights, conversations, minigames and battles all refuse. Being able to top the creature up
while nothing decays would make it a cheat rather than a pause.

### How it's enforced

One gate, at the top of [`Pet::tick()`](../main/game/sim/pet.cpp):

```cpp
if (frozen_) return;
```

Everything time-driven lives below that line, and every path into the simulation goes through
`tick()` — the live frame loop, the light-sleep loop, and `Pet::boot()`'s offline catch-up.
That last one is what makes the guarantee survive power cycles: a frozen pet that spends a week
switched off replays the week through `tick()`, which does nothing, and `markSaved()` then
re-stamps `lastUpdate` so the stretch is never seen as elapsed time again. There is no
"frozen since" timestamp to keep in sync, because there is nothing to subtract.

Care actions funnel through `Pet::careBlocked()`, which refuses and arms the usual "no" wiggle,
so a tap on a greyed-out button still answers. `grant_training()` returns an empty result as a
backstop, though the Activities and Battle menu entries are already shut.

### Interaction with deep sleep

`power_service_timer_wake()` checks `pet_frozen_saved()` **first** and re-sleeps immediately —
before loading the creature and personality registries (~15 KB plus file IO) — since there is
no catch-up to run and no threshold that could newly trip. The mode's whole premise is a long
absence, so its polls should be the cheapest ones the firmware does. The PWR key still wakes
the board instantly, so a freeze is never a lockout.

### Where it shows

| Surface | Frozen state |
|---|---|
| Home | Pause disc in the badge slot (outranks the mood cloud and the attention "!"), creature holds position, whole action bar greyed, `Care paused - Settings > Game` caption |
| Menu | Battle + Activities greyed, hint points at the toggle |
| Stats | `- PAUSED` beside the CARE heading |
| Settings → GAME | Speed reads `Now: paused`; the toggle row itself |

Persisted in its **own NVS key** (`frzn`), like `slpAlert` above — no `PET_VERSION` bump, no
wiped pets. A newly hatched egg always starts running, whatever the previous creature was doing.

## PWR button (GPIO6, active-low)

All gestures are classified on **release** (see `PowerManager::update`):

| Gesture              | Active mode          | Light-sleep mode |
|----------------------|----------------------|------------------|
| Short press (<0.8 s) | → light sleep        | → wake           |
| Hold ≥0.8 s          | → deep sleep         | → wake           |
| Hold ≥3 s            | → power off          | → wake           |

The press that wakes from light sleep is *consumed* so it isn't also read as a hold. On deep
sleep entry we wait for the button to be released before arming the GPIO wake, so the board
doesn't instantly re-wake on the same press.

## Hardware caveats (verify on device)

- **Power latch behaviour** (GPIO7) is inferred from the stock Waveshare driver, not a
  schematic. Confirm the board stays powered through deep sleep and that a PWR press wakes it.
- **PWR-key idle level:** we enable an internal pull-up as a backstop; deep-sleep GPIO wake
  assumes the pin idles high and is pulled low on press.
- **Recovery:** if a wake ever misbehaves, a USB reflash (`idf.py -p COM3 flash`) always
  recovers the board — deep sleep does not block the bootloader.

## Deferred

- 80 MHz DFS for light sleep (PM framework) — see above.
- Adaptive poll interval (poll more often when a stat is near a threshold).
- A menu entry for sleep, if button-only proves unintuitive.
