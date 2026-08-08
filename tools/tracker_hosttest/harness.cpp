// Host harness for tracker module playback: loads a module through the engine's own Decoder
// adapter, renders it, reports statistics and writes a WAV so the output can be measured on a
// PC rather than guessed at from a serial log.
//
// WHY THIS EXISTS. Module playback fails MUSICALLY -- a wrong pitch, a wrong tempo, an
// instrument that never sounds -- and none of that is visible in a device log. But
// engine/audio/tracker.cpp and libxmp-lite depend on nothing from ESP-IDF except two allocator
// calls and the logging macros, so with the shims in shim/ the whole path compiles and runs on
// the host, where the output is a WAV.
//
// It is also a FUZZER (--fuzz), which matters more now than it did: a module is untrusted input
// off a player's SD card, and libxmp is the thing parsing it.
//
// Nothing in the firmware build references this. It is a development tool.
//
// Build (MSYS2 mingw64 g++; any host compiler will do). Note that libxmp-lite's own sources
// come along, which is what makes this a test of the real playback path and not a mock:
//
//   SRC=main/game/engine/audio
//   XMP=components/libxmp-lite
//   g++ -std=gnu++20 -O2 -DLIBXMP_CORE_PLAYER -DLIBXMP_STATIC \
//       -I tools/tracker_hosttest/shim -I $SRC -I $XMP/include -I $XMP/src -I $XMP/src/loaders \
//       tools/tracker_hosttest/harness.cpp $SRC/tracker.cpp $XMP/src/*.c $XMP/src/loaders/*.c \
//       -o harness
//
// Run, on generated fixtures or on any real module:
//
//   python tools/make_test_module.py build/test_modules
//   ./harness build/test_modules/scale.mod build/test_modules/scale.xm
//   python tools/tracker_hosttest/analyse.py build/test_modules
//   ./harness --fuzz build/test_modules/test.xm 1500
//
// harness checks "does it load, decline non-modules, and produce real audio"; analyse.py checks
// "are the notes and the tempo right". See docs/sound-engine.md.

#include "decoder.hpp"
#include "audio.hpp"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <string>

using namespace audio;

static void write_wav(const char* path, const std::vector<int16_t>& pcm)
{
    FILE* f = fopen(path, "wb");
    if (!f) return;
    const uint32_t dataBytes = (uint32_t)(pcm.size() * 2);
    const uint32_t rate = kMixRate;
    fwrite("RIFF", 1, 4, f);
    uint32_t v = 36 + dataBytes; fwrite(&v, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    v = 16; fwrite(&v, 4, 1, f);
    uint16_t w = 1; fwrite(&w, 2, 1, f);          // PCM
    w = 2; fwrite(&w, 2, 1, f);                   // stereo
    fwrite(&rate, 4, 1, f);
    v = rate * 4; fwrite(&v, 4, 1, f);            // byte rate
    w = 4; fwrite(&w, 2, 1, f);                   // block align
    w = 16; fwrite(&w, 2, 1, f);                  // bits
    fwrite("data", 1, 4, f);
    fwrite(&dataBytes, 4, 1, f);
    fwrite(pcm.data(), 1, dataBytes, f);
    fclose(f);
}

static Decoder* open_module(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) return nullptr;
    Decoder* d = module_try(f);
    if (!d) fclose(f);
    return d;
}

static int probe(const char* path, const char* wavOut)
{
    printf("\n================ %s ================\n", path);

    Decoder* dec = open_module(path);
    if (!dec) { printf("  FAIL: module_try declined it\n"); return 1; }
    printf("  loaded; rate %u, chans %u, block %d\n",
           (unsigned)dec->rate(), dec->chans(), dec->maxBlockSamples());

    dec->setLooping(true);

    const int wantFrames = kMixRate * 6;          // six seconds
    std::vector<int16_t> pcm;
    std::vector<int16_t> buf(dec->maxBlockSamples());
    int frames = 0, calls = 0, earlyStops = 0;
    while (frames < wantFrames) {
        const int n = dec->decode(buf.data(), (int)buf.size());
        calls++;
        if (n <= 0) { earlyStops++; break; }
        pcm.insert(pcm.end(), buf.begin(), buf.begin() + n);
        frames += n / 2;
    }

    // `nonSilent` is the real question: a module that loads but never triggers a note renders
    // a buffer of zeros, which every other check here would happily pass.
    long long sumSq = 0;
    int peak = 0, nonSilent = 0;
    for (int16_t s : pcm) {
        const int a = abs((int)s);
        if (a > peak) peak = a;
        if (a > 64) nonSilent++;
        sumSq += (long long)s * s;
    }
    const double rms = pcm.empty() ? 0.0 : sqrt((double)sumSq / (double)pcm.size());
    const double nsPct = nonSilent * 100.0 / (pcm.empty() ? 1 : pcm.size());
    printf("  rendered %d frames in %d calls (%d early stops)\n", frames, calls, earlyStops);
    printf("  peak %d (%.1f%% FS), rms %.0f, non-silent samples %.1f%%\n",
           peak, peak * 100.0 / 32767.0, rms, nsPct);

    int rc = 0;
    if (frames < wantFrames) { printf("  FAIL: stopped early -- a looping module must not end\n"); rc = 1; }
    if (peak < 1000)         { printf("  FAIL: essentially silent\n"); rc = 1; }
    if (nsPct < 5.0)         { printf("  FAIL: almost all samples are silent\n"); rc = 1; }
    if (rc == 0) printf("  OK\n");

    if (wavOut) { write_wav(wavOut, pcm); printf("  wrote %s\n", wavOut); }
    delete dec;
    return rc;
}

// module_try is offered EVERY file by decoder_open(), including WAVs and MP3s, so it declining
// things that are not modules is load-bearing rather than cosmetic.
static int probe_negative(const char* path)
{
    printf("\n--- must DECLINE: %s\n", path);
    Decoder* d = open_module(path);
    if (d) { printf("  FAIL: claimed a file that is not a module\n"); delete d; return 1; }
    printf("  declined  OK\n");
    return 0;
}

// Run a whole corpus through the adapter: load each, render a second, count outcomes. Used
// against libxmp's own test-data tree, which is ~500 real modules across all four formats plus
// a directory of deliberately malformed ones. Far more real-world variety than hand-made
// fixtures can offer, and the only thing that actually FAILS here is a crash or a hang.
//
// Expect a healthy number of both other outcomes:
//   declines -- the test-dev/data/f/ tree exists precisely to be rejected.
//   silent   -- OpenMPT's regression modules are frequently titled "Should Stay Silent": they
//               test that a player does NOT make a sound in some edge case. Reported rather
//               than counted as failure, but worth reading the names before worrying.
static int batch(int count, char** paths)
{
    // A corpus sweep is thousands of characters of paths, which overruns the command line on
    // Windows -- so accept "@listfile" (one path per line) as well as literal arguments.
    std::vector<std::string> list;
    if (count == 1 && paths[0][0] == '@') {
        FILE* lf = fopen(paths[0] + 1, "r");
        if (!lf) { printf("cannot open list %s\n", paths[0] + 1); return 1; }
        char line[512];
        while (fgets(line, sizeof line, lf)) {
            std::string s(line);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
            if (!s.empty()) list.push_back(s);
        }
        fclose(lf);
    } else {
        for (int i = 0; i < count; i++) list.push_back(paths[i]);
    }

    int loaded = 0, declined = 0, silent = 0;
    for (size_t i = 0; i < list.size(); i++) {
        Decoder* d = open_module(list[i].c_str());
        if (!d) { declined++; continue; }
        loaded++;

        d->setLooping(true);
        std::vector<int16_t> buf(d->maxBlockSamples());
        int peak = 0, frames = 0;
        while (frames < kMixRate) {                 // one second is plenty to hear a note
            const int n = d->decode(buf.data(), (int)buf.size());
            if (n <= 0) break;
            for (int k = 0; k < n; k++) { const int a = abs((int)buf[k]); if (a > peak) peak = a; }
            frames += n / 2;
        }
        if (peak < 256) { silent++; printf("  SILENT: %s\n", list[i].c_str()); }
        delete d;
    }
    printf("\nbatch: %d files -> %d loaded, %d declined, %d loaded-but-silent\n",
           (int)list.size(), loaded, declined, silent);
    // Only a crash or a hang fails this; both show up as the process not getting here.
    return 0;
}

// --- fuzzing ------------------------------------------------------------------------------
static uint32_t rng_state = 0x1234567u;
static uint32_t rng()
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static int fuzz(const char* path, int iterations)
{
    FILE* f = fopen(path, "rb");
    if (!f) { printf("cannot open %s\n", path); return 1; }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> orig((size_t)size);
    if (fread(orig.data(), 1, orig.size(), f) != orig.size()) { fclose(f); return 1; }
    fclose(f);

    printf("\n=== fuzzing %s (%ld bytes, %d iterations) ===\n", path, size, iterations);

    const std::string tmp = std::string(path) + ".fuzz.tmp";
    int loaded = 0, rejected = 0;

    for (int it = 0; it < iterations; it++) {
        std::vector<uint8_t> v = orig;
        switch (rng() % 3) {
            case 0: v.resize(rng() % (v.size() + 1)); break;          // truncate
            case 1: {                                                 // flip a few bytes
                const int n = 1 + (int)(rng() % 8);
                for (int k = 0; k < n && !v.empty(); k++) v[rng() % v.size()] = (uint8_t)(rng() & 0xFF);
                break;
            }
            default: {                                                // 0xFF: max out counts
                const int n = 1 + (int)(rng() % 4);                   // and lengths, the
                for (int k = 0; k < n && !v.empty(); k++)              // dangerous case
                    v[rng() % v.size()] = 0xFF;
                break;
            }
        }

        FILE* o = fopen(tmp.c_str(), "wb");
        if (!o) { printf("cannot write temp\n"); return 1; }
        if (!v.empty()) fwrite(v.data(), 1, v.size(), o);
        fclose(o);

        if (Decoder* d = open_module(tmp.c_str())) {
            loaded++;
            // Play it too. A corrupt module that LOADS is the more dangerous case, because it
            // then goes on to index patterns and samples for real.
            d->setLooping(true);
            std::vector<int16_t> buf(d->maxBlockSamples());
            for (int b = 0; b < 40; b++) if (d->decode(buf.data(), (int)buf.size()) <= 0) break;
            delete d;
        } else {
            rejected++;
        }
    }
    remove(tmp.c_str());
    printf("  survived: %d variants loaded and played, %d rejected\n", loaded, rejected);
    return 0;
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        printf("usage: harness <module> [<module>...]\n"
               "       harness --decline <non-module-file> [...]\n"
               "       harness --batch <module> [...]        (corpus sweep; declines are OK)\n"
               "       harness --fuzz <module> [iterations]\n");
        return 2;
    }

    if (strcmp(argv[1], "--batch") == 0) return batch(argc - 2, argv + 2);

    if (strcmp(argv[1], "--fuzz") == 0) {
        if (argc < 3) { printf("--fuzz needs a seed module\n"); return 2; }
        const int iters = (argc > 3) ? atoi(argv[3]) : 2000;
        const int rc = fuzz(argv[2], iters);
        printf("\n%s\n", rc ? "*** FUZZ FAILED ***" : "fuzz completed without a crash");
        return rc;
    }

    if (strcmp(argv[1], "--decline") == 0) {
        int rc = 0;
        for (int i = 2; i < argc; i++) rc |= probe_negative(argv[i]);
        printf("\n%s\n", rc ? "*** FAILURES ***" : "all declined as expected");
        return rc;
    }

    int rc = 0;
    for (int i = 1; i < argc; i++) {
        std::string out = std::string(argv[i]) + ".wav";
        rc |= probe(argv[i], out.c_str());
    }
    printf("\n%s\n", rc ? "*** FAILURES ***" : "all modules OK");
    return rc;
}
