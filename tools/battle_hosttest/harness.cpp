// Host harness for the battle core's PACING -- how the two ATB gauges are spaced in time.
//
// WHY THIS EXISTS. "Both fighters attack at the same moment" is a complaint about rhythm,
// and rhythm is invisible in a serial log: the events all arrive, in the right order, with
// the right damage. What is wrong is the *spacing* between them, which only shows up once
// you timestamp a few thousand actions and look at the distribution. battle.cpp depends on
// nothing from ESP-IDF but the log macros and (transitively) esp_random, so with the shims
// in shim/ the real core compiles and runs on the host, where the spacing is measurable.
//
// It runs auto-vs-auto fights, timestamps every AttackStart, and reports:
//
//   clump%  fraction of gaps shorter than a quarter of the mean gap. This is the lockstep
//           signature: when the sides fire in matched pairs, half the gaps are ~0 and half
//           are a full charge cycle, so clump% sits near 50. Evenly interleaved sides put
//           it near 0.
//   CV      coefficient of variation of the gaps. High = irregular spacing, but high on its
//           own is ambiguous -- lockstep is also "irregular". Read it next to clump%.
//   p05/p50/p95  the gap distribution itself, in seconds.
//
// Nothing in the firmware build references this. It is a development tool.
//
// Build (MSYS2 mingw64 g++; any host compiler will do). Put mingw64/bin on PATH first --
// cc1plus needs the DLLs there, and invoking g++ by absolute path is not enough: it dies
// with exit 1 and no diagnostic at all.
//
//   export PATH=/c/msys64/mingw64/bin:$PATH
//
//   g++ -std=gnu++20 -O2 -I tools/battle_hosttest/shim -I main/game \
//       tools/battle_hosttest/harness.cpp main/game/battle/battle.cpp -o harness
//
// Run:
//   ./harness            # the pacing report
//   ./harness --selftest # the core's own balance self-test, on the host
//
// See docs/battle-system.md.

#include "battle/battle.hpp"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>
#include <algorithm>

static const float DT = 0.023f;   // ~44 fps, the device's measured frame time

static Combatant mk(const char* name, uint8_t attr, uint32_t hp,
                    uint32_t str, uint32_t end, uint32_t agi, uint32_t intel, float skill)
{
    Combatant c{};
    strncpy(c.name, name, sizeof c.name - 1);
    c.attribute = attr;
    c.stat[STAT_MAXHP] = hp; c.stat[STAT_STR] = str; c.stat[STAT_END] = end;
    c.stat[STAT_AGI] = agi;  c.stat[STAT_INT] = intel;
    c.maxHp = hp; c.hp = hp; c.aiSkill = skill; c.spriteIdx = -1;
    return c;
}

struct Action { float t; int side; };

// One auto-vs-auto fight, timestamping every action start.
static void run(Combatant p, Combatant e, uint32_t seed, std::vector<Action>& out, int* winner)
{
    Battle b; b.begin(p, e, seed); b.setAuto(0, true); b.setAuto(1, true);
    float t = 0.0f;
    BattleEventRec ev;
    for (int guard = 0; !b.finished() && guard < 40000; guard++) {
        b.update(DT);
        t += DT;
        while (b.pollEvent(ev))
            if (ev.type == BattleEvent::AttackStart) out.push_back({ t, ev.actor });
    }
    if (winner) *winner = b.winner();
}

static float pct(std::vector<float>& v, float q)
{
    if (v.empty()) return 0.0f;
    size_t i = (size_t)(q * (float)(v.size() - 1) + 0.5f);
    return v[std::min(i, v.size() - 1)];
}

struct Report { float clump, cv, p05, p50, p95, mean; int side0Wins, fights; long actions; };

static Report series(Combatant p, Combatant e, int n)
{
    std::vector<float> gaps;
    Report r{}; r.fights = n;
    for (int i = 0; i < n; i++) {
        std::vector<Action> acts;
        int w = -1;
        run(p, e, 1000u + (uint32_t)i * 7919u, acts, &w);
        if (w == 0) r.side0Wins++;
        r.actions += (long)acts.size();
        for (size_t k = 1; k < acts.size(); k++) gaps.push_back(acts[k].t - acts[k - 1].t);
    }
    if (gaps.empty()) return r;

    double sum = 0.0;
    for (float g : gaps) sum += g;
    r.mean = (float)(sum / (double)gaps.size());

    double var = 0.0;
    for (float g : gaps) var += (g - r.mean) * (g - r.mean);
    r.cv = r.mean > 0.0f ? (float)(sqrt(var / (double)gaps.size()) / r.mean) : 0.0f;

    int clumped = 0;
    for (float g : gaps) if (g < r.mean * 0.25f) clumped++;
    r.clump = 100.0f * (float)clumped / (float)gaps.size();

    std::sort(gaps.begin(), gaps.end());
    r.p05 = pct(gaps, 0.05f); r.p50 = pct(gaps, 0.50f); r.p95 = pct(gaps, 0.95f);
    return r;
}

static void row(const char* label, Combatant p, Combatant e, int n)
{
    Report r = series(p, e, n);
    printf("%-26s clump %5.1f%%  CV %4.2f  gaps p05/p50/p95 %5.2f/%5.2f/%5.2f s"
           "  mean %4.2f  side0 %d/%d  %ld actions\n",
           label, r.clump, r.cv, r.p05, r.p50, r.p95, r.mean, r.side0Wins, r.fights, r.actions);
}

int main(int argc, char** argv)
{
    if (argc > 1 && strcmp(argv[1], "--selftest") == 0) { battle_selftest(); return 0; }

    printf("battle pacing report  (dt=%.3fs, auto vs auto)\n", DT);
    printf("clump%% = gaps shorter than a quarter of the mean; ~50%% means the sides fire in\n"
           "matched pairs (lockstep), near 0%% means they are evenly interleaved.\n\n");

    // The mirror match is the worst case: identical AGI means identical charge rates, so
    // nothing in the fight itself can pull the two gauges apart.
    row("mirror (equal AGI)",
        mk("Alpha", ATTR_DATA, 60, 10, 8, 12, 9, 0.7f),
        mk("Beta",  ATTR_DATA, 60, 10, 8, 12, 9, 0.7f), 40);

    // A near-tie: different rates, but too close to desync on their own within a fight.
    row("near-tie (AGI 12 vs 13)",
        mk("Alpha", ATTR_DATA, 60, 10, 8, 12, 9, 0.7f),
        mk("Beta",  ATTR_DATA, 60, 10, 8, 13, 9, 0.7f), 40);

    // Genuinely different charge rates -- and note how far apart the AGI has to be to get
    // them. Fill time is interpolated over 0..9999, so this row is 2.91s vs 1.95s, while any
    // AGI a real pet is likely to have trained sits within a few ms of the slow end. That is
    // why the lockstep was never a mirror-match edge case: every fight is a mirror on rate.
    row("wide gap (AGI 400 vs 8000)",
        mk("Alpha", ATTR_DATA, 220, 14, 10,  400, 12, 0.7f),
        mk("Beta",  ATTR_DATA, 220, 14, 10, 8000, 12, 0.7f), 40);

    // Near the AGI cap: the shortest charge cycle in the game (1.34s), where the beat between
    // actions is proportionally the largest and so most likely to distort the pacing.
    row("fast pair (AGI 9500)",
        mk("Alpha", ATTR_DATA, 400, 30, 20, 9500, 20, 0.7f),
        mk("Beta",  ATTR_DATA, 400, 30, 20, 9500, 20, 0.7f), 40);

    return 0;
}
