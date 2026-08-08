#include "decoder.hpp"
#include "esp_log.h"
#include <cstring>
#include <strings.h>   // strcasecmp

// Format dispatch. Deliberately content-sniffing rather than extension-driven: this engine's
// files come from mod packs and SD cards that players assemble by hand, where a re-encoded
// clip keeps its old extension routinely. The extension is used only to decide which format
// to TRY FIRST, so the common case costs one attempt and the mislabelled case still plays.

namespace audio {

static const char* TAG = "AUD/DEC";

static const char* ext_of(const char* path)
{
    const char* dot = strrchr(path, '.');
    return dot ? dot + 1 : "";
}

// Tracker module extensions libxmp-lite handles. `.mod` is also written as a PREFIX on Amiga
// ("mod.songname"), which is still how much of the archive material is named, so both
// spellings are recognised.
static bool is_module_ext(const char* path)
{
    const char* e = ext_of(path);
    if (strcasecmp(e, "xm")  == 0 || strcasecmp(e, "mod") == 0 ||
        strcasecmp(e, "s3m") == 0 || strcasecmp(e, "it")  == 0) return true;

    const char* slash = strrchr(path, '/');
    const char* base  = slash ? slash + 1 : path;
    return strncasecmp(base, "mod.", 4) == 0;
}

bool decoder_is_module(const char* path) { return path && is_module_ext(path); }

bool decoder_handles_extension(const char* path)
{
    // No ".adpcm": IMA ADPCM is only supported INSIDE a WAV container (wav.cpp), and nothing
    // here can decode a headerless one. Advertising it registered such files as sounds that
    // could never open -- and, because a stream fallback re-sniffs on every play, each attempt
    // cost a file open, an MP3 resync scan and a whole-file copy into PSRAM before failing.
    const char* e = ext_of(path);
    return strcasecmp(e, "wav") == 0 || strcasecmp(e, "mp3") == 0 || is_module_ext(path);
}

Decoder* decoder_open(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) {
        // SAY SO. A sound whose file is missing or mis-pathed is the most likely mod authoring
        // mistake -- and by far the hardest to diagnose, because every layer above degrades
        // gracefully: play() returns kNoHandle, the voice frees itself, and the game carries on
        // in silence with nothing anywhere to say why. Note that `file` in a manifest is
        // relative to the sounds/ directory the manifest itself is in, which is the detail
        // people get wrong.
        ESP_LOGW(TAG, "%s: cannot open (missing file, or a `file` path that is not relative "
                      "to the sounds/ dir?)", path);
        return nullptr;
    }

    const char* e = ext_of(path);

    // Sniff order. Every factory rejects a stream that is not its format, so this only decides
    // how many attempts the common case costs. module_try goes LAST for everything else: it is
    // the loosest test of the three, since several module formats have no magic number at
    // offset 0 (a MOD's tag sits at byte 1080), so it must not get first refusal on a file
    // another decoder would have claimed.
    Decoder* (*order[3])(FILE*);
    if (is_module_ext(path))             { order[0] = module_try; order[1] = wav_try;    order[2] = mp3_try; }
    else if (strcasecmp(e, "mp3") == 0)  { order[0] = mp3_try;    order[1] = wav_try;    order[2] = module_try; }
    else                                 { order[0] = wav_try;    order[1] = mp3_try;    order[2] = module_try; }

    // Each factory leaves the file position undefined when it declines, so rewind between
    // attempts. On success the decoder owns the FILE*.
    for (auto* tryOpen : order) {
        fseek(f, 0, SEEK_SET);
        if (Decoder* d = tryOpen(f)) return d;
    }

    fclose(f);

    // Ogg has a distinctive magic and is a format people WILL bring to this engine, so name it
    // specifically instead of letting it fall into the generic "unsupported" bucket -- the
    // difference between a five-second diagnosis and an hour of guessing.
    if (strcasecmp(e, "ogg") == 0 || strcasecmp(e, "opus") == 0)
        ESP_LOGW(TAG, "%s: Ogg/Opus is not built in (see docs/sound-engine.md); use WAV or MP3", path);
    else
        ESP_LOGW(TAG, "%s: no built-in decoder recognised this file", path);
    return nullptr;
}

}   // namespace audio
