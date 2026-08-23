#include "battery.hpp"
#include "drivers.hpp"    // BAT_analogVolts (filtered by drivers/BAT_Driver)
#include "util.hpp"       // clampf
#include "esp_log.h"
#include "nvs.h"
#include <atomic>
#include <cmath>

static const char* TAG = "BAT";

// Calibration lives in its own NVS namespace rather than the game's: it describes the
// HARDWARE (this pack, this board's divider), not the save, so a factory reset has no
// business clearing it, and the driver task can write it without touching SaveStore.
static const char* NVS_NS = "bat";

// ---------------------------------------------------------------------------------------
// Percentage from voltage
// ---------------------------------------------------------------------------------------
// Voltage is a poor fuel gauge -- a single cell spends most of its life between 3.7 and
// 3.9 V -- but it is all this board offers (no coulomb counter). What it must not be is
// LINEAR: the old straight 3.0-4.2 V ramp called a half-empty 3.7 V pack 58%, so the gauge
// sat near the middle for days and then fell off a cliff. This is the usual single-cell
// rest curve in 5% steps, interpolated between points.
struct CurvePt { float v; int pct; };
static const CurvePt CURVE[] = {
    {4.20f,100}, {4.15f, 95}, {4.11f, 90}, {4.08f, 85}, {4.02f, 80}, {3.98f, 75},
    {3.95f, 70}, {3.91f, 65}, {3.87f, 60}, {3.85f, 55}, {3.84f, 50}, {3.82f, 45},
    {3.80f, 40}, {3.79f, 35}, {3.77f, 30}, {3.75f, 25}, {3.73f, 20}, {3.71f, 15},
    {3.69f, 10}, {3.61f,  5}, {3.27f,  0},
};
static const int   CURVE_N   = (int)(sizeof(CURVE) / sizeof(CURVE[0]));
static const float CURVE_TOP = 4.20f;   // the voltage the curve calls 100%

// The curve above is written for a textbook cell measured at rest. This board measures
// neither: the reading is taken through an uncalibrated divider, while the board is running,
// and it sits ~150 mV higher with a charger attached than without. Rather than guess at any
// of that, the gauge learns the two voltages that matter and slides the curve to meet them:
//
//   vFull      what a FULL pack reads while charging  (the charger's plateau)
//   vLoadFull  what a FULL pack reads on battery      (same pack, our own load instead)
//
// Both are measured on the device (see below), so the curve ends up calibrated to the pack
// that is actually fitted. Until they are, these priors stand in -- the difference between
// them is the plug-in offset the gauge has to cancel, so they must not start out equal.
static const float V_FULL_PRIOR     = 4.20f;
static const float V_LOADFULL_PRIOR = 4.08f;
// Sanity bounds. A learned value outside these is a misreading, not a pack.
static const float V_FULL_MIN = 4.00f, V_FULL_MAX = 4.40f;
static const float V_LOAD_MIN = 3.85f, V_LOAD_MAX = 4.30f;

// `anchor` = what a full pack reads in the CURRENT state. Shifting by it does two jobs at
// once: it calibrates the divider, and it cancels the charging offset (because the reading
// and the anchor both step up by the same ~150 mV when a charger is attached, the percentage
// stays put across a plug-in instead of jumping).
static int pct_from_volts(float v, float anchor)
{
    v += CURVE_TOP - anchor;

    if (v >= CURVE[0].v)            return 100;
    if (v <= CURVE[CURVE_N - 1].v)  return 0;
    for (int i = 1; i < CURVE_N; i++) {
        if (v > CURVE[i].v) {       // between point i (below) and i-1 (above)
            const float span = CURVE[i - 1].v - CURVE[i].v;
            const float t    = span > 0.0f ? (v - CURVE[i].v) / span : 0.0f;
            return CURVE[i].pct + (int)(t * (CURVE[i - 1].pct - CURVE[i].pct) + 0.5f);
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------------------
// Is a charger attached?
// ---------------------------------------------------------------------------------------
// Two things give it away in the voltage, and they cover different situations:
//
//   1. The STEP. Plugging in swings the cell current from "supplying the board" to
//      "absorbing a few hundred mA", and the terminal jumps by well over 100 mV within the
//      filter's couple of seconds. A discharging pack never does that, so a rise above the
//      recent floor is a plug-in event. (This is the case the user actually sees.)
//   2. The CLIMB. If the board BOOTS with the charger already attached there is no step to
//      catch, and the constant-current climb is far too slow for (1). But a pack that is
//      being discharged never gains voltage over minutes, so a sustained rise says charger.
//      A big load coming off (leaving a minigame) also lifts the reading once, which is why
//      the climb has to continue into a second window before it counts.
// Plus the obvious: a reading no pack can produce on its own means a charger. The ceiling
// for "on its own" is not a textbook 4.2 V, it is vLoadFull -- BY DEFINITION the highest this
// board reads on battery, since it is what a FULL pack reads under our own load. Margin on
// top covers the load getting lighter than it was when that was measured (light sleep turns
// the backlight off, which lifts the reading). This is the only test that fires when the
// board BOOTS on a charger that has already finished: no step to catch, and a plateau is by
// definition not climbing.
static const float CHG_ABS_CAP    = 4.25f;   // fallback ceiling, before anything is learned
static const float CHG_ABS_MARGIN = 0.10f;   // above vLoadFull -> cannot be the pack alone
static const float CHG_STEP_V  = 0.07f;   // rise above the recent floor that reads as plug-in
static const float DIS_DROP_V  = 0.05f;   // fall from the recent ceiling that reads as unplug
static const float ENV_TAU_S   = 120.0f;  // how slowly floor/ceiling creep back toward the reading
static const float WIN_S       = 150.0f;  // long-window sampling interval
static const float WIN_RISE_V  = 0.03f;   // per-window rise that counts as climbing
static const int   WIN_NEEDED  = 2;       // consecutive climbing windows before it's a charger

// ---------------------------------------------------------------------------------------
// "Full", and where the calibration comes from
// ---------------------------------------------------------------------------------------
// A charger's last phase holds the pack at a fixed voltage while the current tapers away, so
// once the reading STOPS CLIMBING near the top, charging is essentially done -- and the
// voltage it stopped at is this board's reading for a full pack. That is worth far more than
// a nominal 4.20 V: it needs no schematic, no divider calibration and no pack datasheet, and
// it is why the gauge can reach 100% on a charger that only ever reads 4.17 V.
//
// Late constant-current charging also looks flat if you squint, hence the tight band over a
// long window: a few mV/minute of climb keeps resetting the clock, a charger holding station
// does not. (Residual noise after the driver's 16-sample average and EMA is ~1-2 mV.)
static const float FULL_FLAT_S    = 300.0f;  // this long without climbing...
static const float FULL_FLAT_BAND = 0.008f;  // ...where "climbing" means more than this
static const float FULL_MIN_V     = 4.05f;   // and only this high up (a stalled charge isn't full)

// The other anchor is measured at the moment its meaning is unambiguous: an unplug from a
// pack we had just called full. Wait for the load step and the filter to settle first.
static const float LOAD_LEARN_S = 60.0f;

// Percentage rate limit. Learned anchors cancel most of the plug-in offset but not all of it
// (charge current tapers, cell resistance rises as the pack empties), so the remainder
// arrives as a slow walk rather than a jump -- which was the original complaint.
static const float PCT_SLEW_PER_S = 1.0f;

// Published state. Each field is written only by battery_poll() (the driver task) and read
// from the render task, so plain atomics are enough -- no lock, and a reader that catches
// the update mid-flight sees a mix of two adjacent samples, which are ~1% apart.
static std::atomic<float> s_volts{0.0f};
static std::atomic<int>   s_pct{-1};
static std::atomic<int>   s_pctRaw{-1};
static std::atomic<bool>  s_charging{false};
static std::atomic<bool>  s_full{false};
static std::atomic<float> s_vFull{V_FULL_PRIOR};
static std::atomic<float> s_vLoadFull{V_LOADFULL_PRIOR};
static std::atomic<bool>  s_calibrated{false};

// Calibration is stored in millivolts: two u16s, written a couple of times per charge cycle
// at most.
static void cal_load(float& vFull, float& vLoadFull, bool& learned)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint16_t mv = 0;
    if (nvs_get_u16(h, "vfull", &mv) == ESP_OK && mv) {
        vFull   = clampf(mv / 1000.0f, V_FULL_MIN, V_FULL_MAX);
        learned = true;
    }
    if (nvs_get_u16(h, "vload", &mv) == ESP_OK && mv)
        vLoadFull = clampf(mv / 1000.0f, V_LOAD_MIN, V_LOAD_MAX);
    nvs_close(h);
}

static void cal_store(const char* key, float v)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u16(h, key, (uint16_t)(v * 1000.0f + 0.5f));
    nvs_commit(h);
    nvs_close(h);
}

void battery_poll(float dt)
{
    const float v = BAT_analogVolts;
    if (v <= 0.5f) return;                 // driver has not produced a reading yet

    static bool  primed   = false;
    static bool  charging = false;
    static bool  full     = false;         // charge plateau reached; latched until unplug
    static float floorV   = 0.0f, ceilV = 0.0f;   // leaky min/max envelope of the reading
    static float pctF     = 0.0f;                 // rate-limited percentage (fractional)
    static float winV     = 0.0f, winT  = 0.0f;   // long-window start voltage + elapsed
    static int   winRises = 0;
    static float flatV    = 0.0f, flatT = 0.0f;   // charge plateau reference + how long it has held
    static float learnT   = 0.0f;                 // >0 = counting down to learning vLoadFull
    static float vFull     = V_FULL_PRIOR;
    static float vLoadFull = V_LOADFULL_PRIOR;

    if (!primed) {                         // first reading seeds everything, nothing to compare to
        primed = true;
        bool learned = false;
        cal_load(vFull, vLoadFull, learned);
        if (vLoadFull > vFull) vLoadFull = vFull;   // full-on-battery cannot read above full-on-charger
        s_vFull.store(vFull);
        s_vLoadFull.store(vLoadFull);
        s_calibrated.store(learned);
        floorV = ceilV = winV = flatV = v;
        charging = (v >= fminf(CHG_ABS_CAP, vLoadFull + CHG_ABS_MARGIN));
        pctF     = (float)pct_from_volts(v, charging ? vFull : vLoadFull);
        ESP_LOGI(TAG, "gauge start: %.3f V -> %d%%%s (full=%.3f loaded=%.3f%s)",
                 v, (int)pctF, charging ? " charging" : "", vFull, vLoadFull,
                 learned ? "" : ", assumed");
    }

    // Envelope: follows the reading instantly in its own direction, creeps back the other
    // way. That makes "floor" the lowest the pack has been recently -- the baseline a
    // plug-in step stands out against -- without it being a permanent all-time low.
    const float k = clampf(dt / ENV_TAU_S, 0.0f, 1.0f);
    if (v <= floorV) floorV = v; else floorV += (v - floorV) * k;
    if (v >= ceilV)  ceilV  = v; else ceilV  += (v - ceilV)  * k;

    // Long-window climb detector (case 2 above).
    winT += dt;
    if (winT >= WIN_S) {
        winRises = (v - winV >= WIN_RISE_V) ? winRises + 1 : 0;
        winV = v;
        winT = 0.0f;
    }

    const float absV = fminf(CHG_ABS_CAP, vLoadFull + CHG_ABS_MARGIN);
    const bool was = charging;
    if (!charging) {
        if (v >= absV || v >= floorV + CHG_STEP_V || winRises >= WIN_NEEDED)
            charging = true;
    } else if (v < absV && v <= ceilV - DIS_DROP_V) {
        charging = false;
    }
    if (charging != was) {
        // Both envelopes restart at the current reading: the pre-plug floor would otherwise
        // re-trigger "charging" the instant we let go of it (the reading now sits far above
        // it), and the charging plateau would look like a fall from the old ceiling.
        floorV = ceilV = winV = flatV = v;
        winT = flatT = 0.0f;
        winRises = 0;
        // Unplugging a pack we had called full is the one moment "what a full pack reads on
        // battery" is knowable, so that is when it gets measured -- after the load step and
        // the filter have settled.
        learnT = (!charging && full) ? LOAD_LEARN_S : 0.0f;
        full   = false;
        ESP_LOGI(TAG, "%s (%.3f V)", charging ? "charger attached" : "on battery", v);
    }

    // Charge plateau -> full, and the charging anchor with it.
    if (!full) {
        if (fabsf(v - flatV) > FULL_FLAT_BAND) { flatV = v; flatT = 0.0f; }   // still moving
        else                                     flatT += dt;
        // The plateau doubles as a charger detector on a device that has never seen a full
        // charge -- and it has to, or an uncalibrated board that BOOTS onto an already-finished
        // charge can never calibrate: there is no step to catch, a plateau is not a climb, and
        // the derived threshold above needs the very calibration it is blocking. A reading that
        // has not moved 8 mV in five minutes this high up cannot be a pack running the board:
        // that falls measurably (the top of the curve is ~40 mV per 5%). Once calibrated the
        // derived threshold covers the boot case properly, so the inference is not needed and
        // is switched off -- it is a bootstrap, not a rule.
        const bool inferCharger = !charging && !s_calibrated.load();
        if (flatT >= FULL_FLAT_S && v >= FULL_MIN_V && (charging || inferCharger)) {
            if (!charging) {
                charging = true;
                floorV = ceilV = winV = v;
                winT = 0.0f;
                winRises = 0;
                ESP_LOGI(TAG, "charger inferred from a flat %.3f V", v);
            }
            full = true;
            const float nv = clampf(v, V_FULL_MIN, V_FULL_MAX);
            const bool  first = !s_calibrated.load();
            const bool  moved = fabsf(nv - vFull) > 0.005f;   // don't rewrite flash for a millivolt
            vFull = nv;
            if (vLoadFull > vFull) vLoadFull = vFull;
            if (first || moved) cal_store("vfull", vFull);
            s_vFull.store(vFull);
            s_vLoadFull.store(vLoadFull);
            s_calibrated.store(true);
            ESP_LOGI(TAG, "charge complete at %.3f V (full anchor)", vFull);
        }
    }

    // ...and the battery-side anchor, one minute after such a charge was unplugged.
    if (learnT > 0.0f) {
        if (charging) {
            learnT = 0.0f;                 // plugged back in: that reading means nothing now
        } else {
            learnT -= dt;
            if (learnT <= 0.0f) {
                vLoadFull = clampf(v, V_LOAD_MIN, V_LOAD_MAX);
                if (vLoadFull > vFull) vLoadFull = vFull;
                cal_store("vload", vLoadFull);
                s_vLoadFull.store(vLoadFull);
                ESP_LOGI(TAG, "full pack reads %.3f V on battery (load anchor)", vLoadFull);
            }
        }
    }

    const int target = full ? 100 : pct_from_volts(v, charging ? vFull : vLoadFull);
    const float step = PCT_SLEW_PER_S * dt;
    pctF += clampf((float)target - pctF, -step, step);

    s_volts.store(v);
    s_pct.store((int)(pctF + 0.5f));
    s_pctRaw.store(target);
    s_charging.store(charging);
    s_full.store(full);
}

BatteryState battery_state()
{
    BatteryState b;
    b.volts      = s_volts.load();
    b.pct        = s_pct.load();
    b.pctRaw     = s_pctRaw.load();
    b.charging   = s_charging.load();
    b.full       = s_full.load();
    b.vFull      = s_vFull.load();
    b.vLoadFull  = s_vLoadFull.load();
    b.calibrated = s_calibrated.load();
    return b;
}
