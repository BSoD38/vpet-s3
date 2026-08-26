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
- **A clock that moved is not an absence.** The elapsed delta is only trusted when it can be
  one: a saved baseline of 0 (written while the RTC read a pre-`CLOCK_YEAR_MIN` date, i.e.
  after it lost power) or a gap over 30 days means the clock was set, reset, or had its
  backup battery changed, and `Pet::boot()` replays *nothing*. Capping such a jump at 24 h
  instead is what once brought a creature back evolved, starving and at 0 HP after nothing
  more than setting the time and power-cycling. For the same reason the clock-setting screen
  re-anchors the baseline with `markSavedAt(clock_epoch(t))` — the time it just wrote, since
  the shared `datetime` snapshot still holds the old one for up to ~100 ms.
- **The ULP can't run the sim.** `tick()` is float-heavy (`expf`, decay rates) and evolution
  reads the creature registry loaded from flash/SD. The ULP-RISC-V has no FPU, ~8 KB of RTC
  RAM, and no access to that data — it would mean a hand-ported fixed-point *duplicate* of
  the whole simulation for negligible battery gain over the approach below.

Instead: **deep sleep with a periodic RTC-timer wake** (CPU wakes briefly every
`POWER_DEEP_POLL_S`, default 15 min) **+ instant GPIO wake on the PWR button**.

## Light sleep

Screen off, simulation still running. Entered by a short PWR press or after the player's
**screen timeout** (Settings -> SCREEN, 15/30/45/60 s, default 60 s) of no input; exited by
any touch or PWR press. If nothing happens for `AUTO_DEEP_US` (15 min total idle) it
**escalates to deep sleep** on its own — light sleep still runs the CPU at 240 MHz, so the
real battery savings only start at deep sleep. (Manual deep sleep is the ~1 s PWR hold
below.)

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

## Screen settings

Two device settings under **Settings → SCREEN**, both stored in their own NVS keys
(`scrOff`, `scrBri`) and loaded by `screen_settings_load()` in `app_main` *before*
`LCD_Init()` — so the panel opens at the player's brightness rather than flashing the
default and correcting itself a moment later.

| Setting | Range | Notes |
|---------|-------|-------|
| Screen timeout | 15 / 30 / 45 / 60 s (default 60) | How long with no touch and no button before light sleep. Read live by `PowerManager::update()`, so a change takes effect on the next frame. |
| Brightness | 20–100 % (default 70) | The floor is deliberate: at 0 the screen is black, and the control that would undo that is *on* the screen. |

Brightness is stored, not applied, by `set_screen_brightness()` — it writes
`LCD_Backlight`, the driver's "lights on" level, and leaves the panel alone. The pet owns
the final duty cycle, because the pet is the only thing that knows whether the creature's
lights are currently off; callers push a change through `Pet::refreshBacklight()`.

That night dim used to be a fixed 25 %. It is now **35 % of the player's brightness**
(floor 5), because a fixed level would have been *brighter* than a lights-on screen set to
the 20 % minimum.

## Battery gauge and charging

The only battery signal that reaches the ESP32 is an ADC on GPIO8 (a divider off the pack,
`drivers/BAT_Driver`). There is **no charge-status line**: the board's charging LED is wired
straight to the charger chip, and nothing tells the MCU that USB is plugged in. So
`game/engine/battery.cpp` reads both the level and the charging state out of that one voltage.

### Level, and why it calibrates itself

Voltage is a poor fuel gauge, but it must at least not be *linear*: the old straight
3.0–4.2 V ramp called a half-empty 3.7 V pack 58%, so the icon sat near the middle for days
and then dropped off a cliff. It now interpolates the usual single-cell rest curve, which is
flat in the middle and steep at both ends.

That curve is written for a textbook cell measured at rest, and this board measures none of
those things. It reads through an uncalibrated divider (a ×3 resistor pair and a fudge
factor), it reads while the board is *running*, and it reads ~150 mV higher with a charger
attached than without. On the observed hardware a full pack reads **4.17–4.18 V charging and
~4.01 V on battery** — so a curve that thinks 4.20 V is full can never reach 100%, and reports
79% for a pack that was just fully charged.

Rather than guess at the divider, the pack or the load, the gauge **learns the two voltages
that matter** and slides the curve onto whichever applies:

| Anchor | What it is | How it's measured |
|--------|------------|-------------------|
| `vFull` | what a full pack reads **while charging** | the charge plateau (below) |
| `vLoadFull` | what the same pack reads **on battery** | 60 s after unplugging a charge that had reached that plateau |

Both are kept in their own NVS namespace (`bat`) — they describe the *hardware*, not the save,
so a factory reset has no business clearing them. Until they're measured, 4.20 / 4.08 stand in;
the About screen marks that with `est`.

This one mechanism fixes three things at once:

- **Reaching 100%.** The charger's own plateau *is* full, whatever it reads.
- **Divider calibration.** Whatever the resistor pair is doing, both anchors absorb it.
- **The plug-in jump.** When a charger is attached, the reading steps up ~150 mV *and* the
  anchor steps up by the same amount, so the two cancel and the percentage stays put. This is
  a measured cancellation, not the assumed offset it replaces.

What's left is the residue — charge current tapers near the top, cell resistance rises as the
pack empties — so the displayed percentage is still rate-limited to 1%/s. Anything the anchors
don't predict arrives as a slow walk instead of a jump.

### Full, and the plateau

A charger's last phase holds the pack at a fixed voltage while the current tapers away. So
once the reading **stops climbing** near the top, charging is essentially done — and that is
both the "charged" state and the moment `vFull` is learned. It needs no schematic, no divider
calibration and no pack datasheet.

The test is 5 minutes without moving more than 8 mV, above 4.05 V. Late constant-current
charging also looks flat if you squint, hence the tight band over a long window: a few
mV/minute of climb keeps resetting the clock, a charger holding station does not. (Residual
noise after the driver's 16-sample average and EMA is only 1–2 mV, so the band can be that
tight.) `full` latches until the charger is removed, so a couple of mV of wobble can't knock
the gauge back off 100%.

On a device that has **never** seen a full charge, the plateau doubles as a charger detector —
and it has to, or a board that boots onto an already-finished charge can never calibrate: no
step to catch, a plateau is not a climb, and the derived threshold in the table below needs the
very calibration it is blocking. A reading that hasn't moved 8 mV in five minutes that high up
cannot be a pack running the board; that falls measurably. Once calibrated the derived
threshold handles the boot case properly and the inference switches off — it's a bootstrap,
not a rule.

### Charging detection

Three signals, covering different situations:

| Signal | What it catches |
|--------|-----------------|
| Reading above `vLoadFull` + 100 mV | `vLoadFull` is *by definition* the highest this board reads on battery (a full pack under our own load), so anything above it plus margin is a charger. Capped at 4.25 V before it has been learned. This is the only test that fires when the board **boots on a charger that has already finished** — no step to catch, and a plateau is by definition not climbing. The margin covers the load getting lighter than it was when the anchor was measured (light sleep turns the backlight off, which lifts the reading). |
| Reading rises ≥ 70 mV above its recent floor | The **plug-in step**: the cell current swings from supplying the board to absorbing half an amp, and the terminal jumps well over 100 mV inside the filter's couple of seconds. A discharging pack never does that. |
| Reading climbs across two consecutive 150 s windows | Booting with the charger **already attached** — no step to catch, and the constant-current climb is far too slow for the row above. A big load coming off (leaving a minigame) also lifts the reading once, which is why the climb has to continue into a second window. |

Unplugging is the mirror of the step: a fall of ≥50 mV from the recent ceiling. Both envelopes
restart on every transition — otherwise the pre-plug floor would immediately re-trigger
"charging" the moment we let go of it.

Consequence: the plug-in the player actually sees is detected in seconds. Booting onto a
charger is caught immediately once calibrated (the reading is above anything the pack can do
alone), or within ~5 minutes of watching it climb if it isn't. Either way it self-corrects,
which is the best a device with no status line can do.

### Where it shows

- **Home HUD:** a lightning bolt left of the battery icon while charging, and the fill turns
  green regardless of level (a red icon with a bolt would read as a fault).
- **Settings → SYSTEM → About:** the raw voltage, the percentage, the charger state, and
  `Full reads` — the two learned anchors (`est` while they're still assumed). Those two numbers
  explain any percentage the gauge reports, so they are the first thing to look at when one
  looks wrong. With the debug overlay on, the `Level` row also shows the value before rate
  limiting.

### The RTC backup cell

The coin cell on the RTC header has no voltage sense anywhere on the board, so **there is no
level to report**. What the PCF85063 does expose is its oscillator-stop flag: set whenever the
chip's clock has stopped, cleared only by writing the seconds register. `PCF85063_Init()` reads
it before anything else can touch it and then clears it, so the About screen can say whether
the cell kept the clock running through the *last* power-off — which is the cell's whole job.
That is the closest thing to a health readout the hardware allows.

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
