# Sound engine

A 4-voice software mixer that owns the I2S DAC, with three ways to make a sound: synthesise
it, play it from RAM, or stream it off storage. Code lives in `main/game/engine/audio/`; the
base game's sound set is `flash_gamedata/sounds/sounds.json`.

## Why it is built this way

The board shipped with `chmorgan/esp-audio-player`, which decodes one file at a time straight
into I2S. That is the wrong shape for a game: a chirp has to land on top of the music that is
already playing, and "one stream" is not something a call site can work around. This engine
replaces it (the component is gone; `drivers/Audio_Driver/PCM5101.h` is now just the board
pinout).

Two design commitments follow from the hardware:

**The mixer never touches the filesystem.** A FAT open costs 7–12 ms here — two whole audio
blocks. If the mixer could block on I/O, every lazy sprite load in the game would be audible
as a dropout. So all slow work happens on a second task behind ring buffers, and `play()`
never blocks the caller either.

**Sounds are data, not code.** Game code calls `audio::play("pet_happy")`; the sound bank
decides what that means. A mod pack can retune the entire game's voice by shipping one JSON
file, using the same override order as foods and creatures.

## Layout

| File | What it is |
|---|---|
| `audio.hpp` | Public API. Read this first. |
| `mixer.cpp` | Voice pool, resampler, I2S sink, the two tasks. |
| `decoder.hpp` / `decoder.cpp` | Format seam and content-sniffing dispatch. |
| `wav.cpp` | PCM 8/16/24/32-bit, 32-bit float, IMA ADPCM. |
| `mp3.cpp` | Helix streaming decoder. |
| `tracker.cpp` | MOD/XM/S3M/IT — a thin adapter over vendored libxmp-lite. |
| `synth.hpp` / `synth.cpp` | Chip voice and RTTTL melody player. |
| `bank.hpp` / `bank.cpp` | Named-sound registry and PSRAM sample cache. |
| `sfx.hpp` | The ids the game itself fires. |

## Runtime shape

```
game loop (core 1)          streamer task (core 0, prio 4)      mixer task (core 0, prio 6)
  audio::play(id) ─┐          opens files, decodes,               every 5.8 ms:
                   │          loads samples to PSRAM,               resample 4 voices
   reserves a voice│          refills ring buffers                  accumulate w/ ramped gain
   returns a handle│                    │                           saturate → i2s_channel_write
                   └──── request ──────►│──── ring buffers ────────►
```

Both tasks sit on core 0 with the drivers, because core 1 is the 60 fps game loop and must not
share with something that wakes every 5.8 ms. The blocking `i2s_channel_write` *is* the
mixer's clock — it returns when DMA takes the block, pacing the loop at exactly real time.

Output is fixed at **44100 Hz stereo**. That rate specifically because MP3 music is almost
always encoded at it, so music passes the resampler at a 1:1 step and stays bit-exact; a lower
mix rate would decimate every song with no lowpass and alias audibly. Mixing costs about 2% of
a core either way — the real cost of audio here is MP3 decode, which the mix rate does not
affect.

### Voices

Four, and voice count is the thing that costs CPU. Voice 0 is reserved for music so an effect
storm can never evict the song; effects prefer voices 1–3 and fall back to voice 0 only when
nothing is playing there. When all voices are busy, the least important and then oldest voice
is stolen, and a sound is dropped rather than stealing something more important than itself.

Resampling is linear-interpolating. The cursor never looks backward into its source — it
carries the two straddling frames in the voice — which is what lets the same code read a ring
buffer, where the previous frame may already be overwritten, as safely as a flat array.

Four voices at full tilt can sum past full scale, and the mix hard-clips rather than running a
limiter that would duck everything whenever an effect fires.

### Power

The DAC clock is cut after 800 ms of silence (a few mA matters on a pet that runs for days),
after writing silence first so the output settles at mid-scale and the speaker does not click.
Screen-off light sleep calls `audio::suspend()`; `power_enter_deep_sleep()` and `power_off()`
call `audio::shutdown()` themselves, so a future caller of either cannot forget to park the DAC
(it is a no-op before `init()`, which is how the boot-time timer-wake path reaches it).

The idle path matters as much as the loud one. The mixer parks on a notification once there is
nothing to mix, and the streamer waits on its request queue: 5 ms while it has a stream to keep
fed or samples to warm, half a second otherwise, and it skips its pass entirely while suspended.
A polling loop here is not free — at 5 ms unconditional it woke 200 times a second and took the
engine lock every time, all night, with the screen off and nothing playing.

### Storage removal

The SD card can be pulled at any moment, and the engine has two live tasks that do not notice
on their own: one holding open file handles on a filesystem that no longer exists, and one
still mixing. So `halt_for_card_change()` calls `audio::shutdown()` **first**, before it even
saves the pet. Without that, a module loaded from the card — fully resident in PSRAM, needing
no further reads — plays happily on through the halt screen and into the reboot, which makes it
look like the card is still there.

Because `shutdown()` is now called from a path that *keeps running* (the 30-second halt prompt)
rather than one that immediately sleeps or reboots, it waits for the mixer to confirm it has
left its loop before deleting the I2S channel. The mixer spends most of its life blocked inside
`i2s_channel_write()` on exactly that channel, so deleting it underneath would be a
use-after-free that the old callers only survived by never returning.

For the same reason the mixer task **parks** at the end of its loop instead of deleting itself.
`shutdown()` deliberately does not wait for the streamer — it may be blocked in a long read on
the card that was just pulled — and the streamer wakes the mixer *by handle* when a load
finishes. Deleting the task would free the TCB that handle points at, so a load completing
during the halt prompt would notify freed memory. Parking keeps the handle valid and costs one
idle task's stack until the reboot that is already seconds away.

Two more places where "the card went away" shows up as something other than an I/O error: a
decoder is deleted by exactly one owner (`attach_stream()` hands ownership back to its caller on
*every* failure path before releasing the slot, or the two would both free it), and a decoder
whose native block does not fit the stream ring is refused outright — `refill_streams()` only
decodes when a whole block fits, so attaching one would leave a voice silently pinned to an
empty ring forever, counting underruns.

**The bounded retry in `refill_streams()` is load-bearing, not defensive.** The natural way to
write a looping stream is "if it ran dry and it loops, rewind and carry on" — and that hangs the
device. `fseek()` on a dead handle still *succeeds* (it sets an offset; it does no I/O), so a
source that has stopped producing gives `decode()==0 → rewind()==true → decode()==0` forever,
and the loop never yields. The streamer runs at priority 4 on core 0, so it starves the driver
task and `sdwatch` beneath it until the task watchdog reboots the board — and with `sdwatch`
starved, the game never even discovers why. Reachable from a yanked card, a zero-length file, or
anything that parses as valid and decodes to nothing. One rewind per dry spell, and only if it
actually yields data; a looping source that comes back empty is logged and ended.

## The three kinds of sound

### `tone` and `melody` — synthesised

This is the format that matters most for a virtual pet, and the base game uses nothing else.
A tone is nine numbers; a melody is one line of RTTTL. The whole base sound set — 28 sounds —
costs about 4 KB inside `base.pak`, against megabytes of WAVs for the same coverage. It also
sounds *right*: square waves and short arpeggios are the idiom for this kind of device.

Synth voices generate directly at the mix rate, so they need no file, no decode, no cache
entry, and no resampling. An effect fires with genuinely zero latency.

RTTTL was chosen because it is a real documented format with thousands of free tunes already
written in it, it is editable in a text field, and it maps exactly onto a monophonic chip
voice. Melodies are monophonic per voice on purpose — for a chord, play two melody sounds and
let the mixer do what it is for.

### `sample` — decoded once into PSRAM

For effects. Playback is a pointer walk, so it is instant and repeatable. Loaded lazily on
first play by the streamer, and warmed in the background at boot (one entry per streamer
iteration) so the first tap is not the one that pays. The cache is bounded at 512 KB total and
192 KB per clip; anything that does not fit is **streamed instead**, never dropped.

### `stream` — decoded while playing

For music and anything long. Costs a ~16 KB ring (about 93 ms of headroom) plus decoder state
instead of the whole clip. Two concurrent streams.

Tracker modules are always forced to this kind regardless of what a manifest asks for: a
module is a *generator*, not a finite clip, so "decode it all into the cache" has no end
condition.

## Tracker modules (MOD and XM)

A module carries its own instruments, so a three-minute looping song costs 50–300 KB instead
of the ~30 MB the same audio would cost as PCM — and it stays musically editable afterwards.
An MP3 is a photograph of a song; a module is the song. For a device with a 3 MB data
partition whose mods arrive on an SD card, that is the whole argument. There is also thirty
years of freely available material in these formats and free editors (OpenMPT, MilkyTracker)
that still open them.

Formats: **MOD, XM, S3M and IT**, via [libxmp-lite](../components/libxmp-lite/) vendored at
`components/libxmp-lite` (MIT, v4.7.2, unmodified). [tracker.cpp](../main/game/engine/audio/tracker.cpp)
is a ~200-line adapter over `xmp_play_buffer()`, which is already a pull API at a caller-chosen
rate — the same shape as this engine's `Decoder` seam.

### Why a library

This replaced a hand-written MOD/XM player of about 1,670 lines. That player worked — verified
correct pitch, correct tempo, zero underruns — but **tracker playback is defined as much by the
bugs of the tracker that created each format as by any specification**, and modules in the wild
depend on those bugs. Matching them is a job measured in years of exposure to real music, which
a from-scratch player validated against synthetic fixtures cannot claim however clean its test
results look. Swapping to libxmp also made S3M and IT free, which the hand-written player had
scoped out *on the grounds of implementation cost* — a reason that evaporates the moment you
link a library that already has them.

It cost **+83 KB of flash** (68% of the app partition still free) and one vendored dependency.

**A module is one voice.** It mixes its own channels internally before handing back finished
stereo, so a 24-channel XM costs one of the four mixer voices, not twenty-four of anything, and
effects still land on top of it. It renders at `kMixRate`, so nothing resamples a module.

### Memory: the one thing that needed work

Tracker samples are randomly accessed (loops, retriggers, sample-offset), so a module must be
fully resident and **cannot** be streamed. libxmp has no allocator hooks and calls plain
`malloc`, so getting a module into PSRAM rather than the ~190 KB of free internal RAM is the
integration's real problem.

`CONFIG_SPIRAM_USE_MALLOC=y` with `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384` handles the big
sample buffers by itself. It does **not** handle the many small allocations: a 24-channel,
60-pattern XM needs a track structure per pattern per channel, and 1440 allocations of a few
hundred bytes each all sit under that 16 KB threshold. Measured, loading one took internal free
heap from 190 KB down to **35 KB** — not enough left for Wi-Fi to come up.

The fix is `PsramBias` in `tracker.cpp`: `heap_caps_malloc_extmem_enable()` moves that threshold
at runtime, so lowering it across the load routes the small allocations to PSRAM too. No change
to libxmp, no project-wide config change. Same module now costs **22 KB internal instead of
155 KB**. The bias is scoped to the load and deliberately *not* to `xmp_start_player`, whose
mixing buffers are touched every audio block and are better off internal.

Measured on device with a real 24-channel, 337 KB-sample XM: 22 KB internal, ~120 KB PSRAM,
0 underruns, mixer peak ~490 µs per 5.8 ms block.

> **Do not disable `CONFIG_SPIRAM_USE_MALLOC`** without giving libxmp another route to PSRAM.

### Using one

Drop it in `/sdcard/sounds/` and it registers under its filename stem, or name it in a
manifest:

```json
{ "id": "bgm_home", "bus": "music", "loop": true, "file": "theme.xm" }
```

Overriding `bgm_home` by id is how a mod pack replaces the built-in chip soundtrack. `loop`
matters here in a way it does not for files: a module decides where its *own* song loops back
to, which is rarely the first row, so the engine tells the player whether to repeat
(`Decoder::setLooping`) rather than rewinding it from outside. With `loop` false, a backward
position jump or running off the end of the order list ends the track.

### Testing it

Module playback fails *musically* — a wrong pitch, a wrong tempo, an instrument that never
sounds — and a serial log shows you none of that. But `tracker.cpp` and libxmp-lite depend on
almost nothing from ESP-IDF (two allocator calls and the logging macros), so with the shims in
`tools/tracker_hosttest/shim/` **the whole playback path compiles and runs on a PC**, where the
output is a WAV you can measure:

```sh
python tools/make_test_module.py build/test_modules      # generate fixtures
SRC=main/game/engine/audio
XMP=components/libxmp-lite
g++ -std=gnu++20 -O2 -DLIBXMP_CORE_PLAYER -DLIBXMP_STATIC \
    -I tools/tracker_hosttest/shim -I $SRC -I $XMP/include -I $XMP/src -I $XMP/src/loaders \
    tools/tracker_hosttest/harness.cpp $SRC/tracker.cpp $XMP/src/*.c $XMP/src/loaders/*.c \
    -o harness
./harness build/test_modules/scale.mod build/test_modules/scale.xm
python tools/tracker_hosttest/analyse.py build/test_modules
```

libxmp's own sources come along, which is what makes this a test of the real path and not a
mock. Four modes:

| Mode | Checks |
|---|---|
| *(default)* | loads, renders six seconds, produces real audio; writes a WAV |
| `--decline <file>` | that a **non**-module is refused — `module_try` is offered every file by `decoder_open()`, so declining WAVs and MP3s is load-bearing |
| `--batch @list.txt` | a whole corpus; declines and silences are reported, only a crash or hang fails |
| `--fuzz <mod> [n]` | truncated and byte-corrupted variants, **played** as well as opened |

`make_test_module.py` emits four fixtures. `test.mod` / `test.xm` stack channels and effects;
`scale.mod` / `scale.xm` play one channel — a chromatic scale, one note every 8 rows — because
that is what can be *measured*. On a multi-channel module "which note is sounding" has no single
answer: a 65 Hz square wave's third harmonic lands on 196 Hz, exactly the G a C-major arpeggio
plays, so a bass note masquerades as a melody note. `analyse.py` tracks the dominant frequency
over time and checks the note sequence and the tempo against that.

The most valuable corpus is **libxmp's own test data** — clone upstream and point `--batch` at
`test/` and `test-dev/`, which is ~500 real modules across all four formats plus OpenMPT's
adversarial regression suite. Two things to expect there rather than worry about: the
`test-dev/data/f/` tree exists precisely to be *rejected*, and many OpenMPT regression modules
are literally titled "Should Stay Silent" because they test that a player does **not** make a
sound in some edge case.

Results at the time of the swap: 452 of 516 loaded and played, 64 declined, no crash or hang;
~1,800 fuzzed variants survived; and pitch/tempo came out **identical** to the hand-written
player it replaced, which is what made the swap safe to make.

Two numbers worth knowing:

- **First-sample peak should be 32512** — the fixtures use a full-scale square wave, so that is
  ±127 widened to 16 bits. A much lower peak on the XM means delta decoding is wrong, which is
  the one mistake in that format that yields something recognisable but badly distorted. The
  player logs this at load, on device too.
- **MOD and XM must agree exactly** on the same scale. They reach it through completely
  different frequency tables (Amiga periods vs linear), so identical output is a real
  cross-check rather than a tautology.

## Formats

| Format | Read | Notes |
|---|---|---|
| WAV PCM 16-bit | yes | Fast path — read straight into the mix buffer. |
| WAV PCM 8 / 24 / 32-bit, 32-bit float | yes | Narrowed to 16-bit on the way in. |
| WAV IMA ADPCM | yes | Fixed 4:1, decode cheaper than the FAT read. Best storage format for effects. |
| MP3 | yes | Helix, fixed-point. Handles ID3v2 and resyncs past junk. |
| MOD, XM, S3M, IT | yes | Tracker modules, via vendored libxmp-lite. See below. |
| Ogg Vorbis / Opus | **no** | See below. |

Dispatch sniffs content, not extensions — modded content arrives with the wrong extension
routinely. The extension only decides which decoder to try first. Note the corollary: IMA
ADPCM is only readable **inside a WAV container**, so a bare `.adpcm` file is not a format
here and is not registered as one.

**On the hand-written WAV reader.** Everything else at this layer is a library (Helix for MP3,
libxmp-lite for modules) on the principle that a mature codec beats a fresh one — so `wav.cpp`
owing nobody is a deliberate exception, and worth saying why. A single-file library
(`dr_wav`) would do the job and carries real fuzzing mileage on malformed headers. It was not
taken because the parser needed here is small and closed (RIFF chunk walk, five sample
formats, IMA blocks), because it must pull through the `Decoder` interface's incremental
`decode(out, max)` contract rather than owning the read loop, and because it is the one format
the whole engine falls back on when everything else declines. That trade is *not* free: this
file is where a header field is trusted, and the review that produced this note found one such
trust (a block size taken from the file with no upper bound) that could wedge a stream. The
mitigation is a bound at the consumer — `attach_stream()` refuses a decoder whose block does
not fit the ring — and the standing rule for the next change here: **treat every value read
out of a header as hostile**.

**On Ogg**: Vorbis needs Tremor (~100 KB of code, ~30–40% of a core to decode) for a format
whose advantage over MP3 is licensing, which is moot when Helix is already vendored here.
Opus is heavier still. Adding either is a `Decoder` subclass and one line in `decoder_open()`
— the seam is built and documented for exactly that. A `.ogg` file currently logs a specific
message naming the format rather than a generic "unsupported".

`module_try` goes **last** in the sniff order for non-module extensions: several module formats
have no magic number at offset 0 (a MOD's tag sits at byte 1080), so it is the loosest test of
the three and must not get first refusal on a file another decoder would have claimed.

## Defining sounds

Scanned weakest-first, so a later root overrides an earlier one by id:

```
/gamedata/sounds/      base game  (packed into base.pak at build time)
/pakN/sounds/          mod packs, later pack wins
/sdcard/sounds/        loose files on the card — the final word
```

Those three are the **global bank**, loaded once at boot. A creature's own sounds are not here
— they live in `<creature dir>/sounds/` and load on demand; see [Creature voices](#creature-voices).

Two ways to define one at each root:

- **`sounds.json`** — an array of sound objects. One FAT open for the whole set, which is why
  the base game uses it.
- **loose files** — any `*.wav` / `*.mp3` / module in the folder registers under its filename
  stem. So "copy `hatch.wav` to `<SD>/sounds/`" is the entire install procedure. A manifest
  entry always wins over a loose file of the same id: explicit beats incidental.

  Ids are matched **case-insensitively**, because FAT hands filenames back in whatever case
  they were written — `BGM_Home.mp3` has to override `bgm_home` or the headline use of loose
  files does not work. A module named the Amiga way (`mod.songname`) registers as `songname`:
  the format marker is on the front of those, so taking the stem before the dot would file
  every one of them under `mod`.

### Fields

Common to every kind:

| Key | Default | Meaning |
|---|---|---|
| `id` | — | Required. An id starting with `_` is treated as a comment entry and skipped. |
| `kind` | inferred | `tone` / `melody` / `sample` / `stream`. Inferred from `rtttl` / `file` extension. |
| `bus` | `sfx` (`music` for streams) | `sfx`, `music`, `ui`. Which volume slider governs it. |
| `gain` | 1.0 | Linear, clamped to 4.0 (past that the mix accumulator overflows). |
| `pitch` | 1.0 | Playback-rate multiplier. |
| `pan` | 0.0 | −1 left … +1 right. |
| `loop` | true on the music bus | Honoured for music too: `false` gives a one-shot sting. |
| `priority` | 128 (200 on music) | Higher survives voice stealing. |
| `pitchVar` | 0.0 | ± random pitch spread. The cheapest fix for machine-gun repetition. |
| `voiced` | false | This sound belongs to the **creature** making it, not to the device. See below. |

`tone`: `wave` (`square`/`pulse`/`triangle`/`saw`/`noise`/`sine`), `freq`, `ms`, `repeat`,
`gapMs`, `step` (frequency ratio between repeats), plus the timbre keys below.

`melody`: `rtttl` (e.g. `"Hatch:d=16,o=6,b=150:c,e,g,c7"`), plus the timbre keys.

Timbre keys, shared by both: `duty` (pulse only), `attack`, `release` (seconds), `slide`
(end/start frequency ratio across each note), `vibHz`, `vibCents`, `vol`.

`sample` / `stream`: `file`, `preload`.

`preload` (default **false**) decodes the clip into the PSRAM cache during the background
warm-up instead of on first play. Opt in for the handful of effects that must be instant the
very first time they fire; leaving it off costs one queued load the first time and nothing
after. It defaults off because a sample-heavy pack with it on spends boot decoding its entire
set over the same SD bus the game is reading sprites from, for sounds that may never play.

`file` is relative to **the `sounds/` directory the manifest is in**, not to the mod root — a
file sitting next to `sounds.json` is `"theme.xm"`, not `"sounds/theme.xm"`. A leading `/`
makes it absolute. This is the field people get wrong, so `decoder_open()` logs loudly when
the resolved path does not exist; every layer above it degrades gracefully, which means a
typo would otherwise be pure silence with nothing anywhere to explain it.

### Example

```json
[
  { "id": "ui_tap", "bus": "ui", "wave": "square", "freq": 1318, "ms": 20,
    "vol": 0.30, "pitchVar": 0.03 },
  { "id": "hatch", "rtttl": "Hatch:d=16,o=6,b=150:c,e,g,c7,8g,8c7",
    "wave": "pulse", "duty": 0.35, "vol": 0.55, "priority": 220 },
  { "id": "bgm_home", "bus": "music", "loop": true, "file": "theme.mp3" }
]
```

That last line is how a mod replaces the built-in chip soundtrack with a real recording: same
id, a file instead of an `rtttl`.

## Creature voices

Some sounds belong to the **device** — a button, a countdown, a fanfare — and some belong to
the **creature** making them: a cry, a chew, a refusal. The second kind should not be the same
noise for every species, and `"voiced": true` is how a sound says which it is.

The constraint that shapes the whole design is the roster. There are hundreds of importable
creatures (see the Digimon importer), and nobody is going to author hundreds of cries. Worse,
a game where only the favourites have a voice sounds *more* broken than one where none do —
the silence becomes conspicuous. So the feature has to work with no authored content at all,
and improve when there is some.

Two layers, and the cheap one carries most of the weight.

### Layer 1: one number per creature

`creature.json` carries a `voicePitch`, and a voiced sound is played through it. That is the
whole mechanism, and it works because a *synthesised* cry played slower is also lower and
longer — pitch on a chip voice is not a transposition, it is a change of size. One float turns
one sound into a roster of them.

Nobody has to write it. When the field is absent it is derived from tier plus a hash of the
creature id: tier picks the band (a hatchling is small and squeaky, a Mega is big and slow)
and the hash spreads creatures *within* their band, so two Champions are not the same creature
wearing a different sprite. It is deterministic and never stored — the same creature sounds
the same on every boot and on every device, and an imported roster gets its voices without
anyone editing 700 files.

```
tier:    egg   train I  train II  child  champion  ultimate  mega  mega+
band:   1.00    1.45      1.32    1.18     1.05      0.94    0.84   0.76      ± 6% by id hash
```

### Layer 2: the creature's own folder

A creature's sounds live in **its own folder** — `<creature dir>/sounds/` — and are loaded when
it starts speaking. Ids inside need no prefix: `pet_happy` in `creatures/agumon/sounds/` *is*
Agumon's `pet_happy`. Dropping `pet_happy.wav` in there is the entire install procedure, which
is the same promise loose files already make for the game's own sounds.

This is the part that makes the system scale, and it is worth being explicit about why the
obvious alternative does not. Registering every creature's sounds in one global bank at boot
fails three separate ways at roster size: hundreds of creatures × ~13 voiced ids is thousands
of entries under one cap; a single `sounds.json` is read under a 64 KB limit, which is a few
hundred entries and not a few thousand; and it is nearly all waste, because **at most two
creatures can be making a noise at once** — the pet, plus an opponent in battle. The sprite
cache in `sim/creatures.hpp` solved the identical problem for art, and this is the same shape.

Three slots are resident at a time (`SOUNDSET_SLOTS`), so a battle can start without evicting
the pet's set first, and the whole feature costs ~48 KB of PSRAM no matter how big the roster
gets. A set is scanned by the *same* manifest and loose-file code as the global bank, so a
creature folder supports everything `sounds.json` does with no second implementation to drift.

### Layer 2b: voice families

`creature.json` also carries an optional `"voice"` naming a **voice family**, for the sounds a
creature has not defined itself. Full resolution order for a voiced id:

```
<creature folder>/pet_happy   →   <species>_pet_happy   →   <family>_pet_happy   →   pet_happy
```

Rungs 2 and 3 live in the global bank. Rung 2 exists for a pack that wants to define several
creatures in one manifest rather than one folder each; rung 3 is the shared voice. The base
game ships four families — `beast`, `machine`, `spirit`, `blob` — covering `pet_happy`,
`pet_sad`, `eat` and `refuse`, and assigns one to each of the 22 base creatures. Neither a
family nor a creature folder is obliged to be complete: an id it does not define falls through.

**A creature's own sound is the one thing the pitch scalar is not applied to.** Writing a sound
into a creature's folder (or naming it after the species) is an author saying "this is the
sound", and pitching what they already tuned would be second-guessing them. A family entry is a
shared starting point, so it *keeps* the per-creature scalar — which is exactly what makes one
authored family sound like N creatures instead of N copies of one.

### How a set gets loaded, and what happens while it hasn't

`Pet::applyVoice()` asks for the set as soon as the species changes, and battle asks for the
opponent's during the intro. The load itself happens on the **streamer** task, because that is
the task that owns every FAT open in this engine — the game never blocks on it.

Until the set arrives, the creature speaks with the family and base sounds. That is the whole
failure mode: **a set that is missing, still loading, or could not be given a slot degrades to
the shared voice, never to silence.** Nothing above the resolution path checks, because there
is nothing useful to do about it.

Eviction is the only part that needs care, and the reason is `Melody`: a synth voice walks the
`Note` array its `Sound` owns, so freeing a set underneath a playing melody would be a
use-after-free rather than merely a wrong noise. The rule that makes it safe is a split of
responsibility rather than a lock:

- The **game thread** claims and releases slots. It is the only thing that starts voices, so it
  is the only thing that can decide nobody is using one — and once it has decided, the answer
  cannot change behind it, because the mixer only ever *frees* voices.
- The **streamer** does the slow work: the directory scan and the frees.
- A slot leaves `READY` the instant it is marked for eviction, and both `sound_at()` and
  `soundset_find()` refuse a slot that is not `READY`. So no voice can be started on a set
  between the decision and the free.

If every slot is busy with a creature that is currently making noise, the new set simply is not
loaded. Three slots against two possible speakers means that is a case which should not arise;
it is handled rather than prevented because "handled" here costs one branch and sounds fine.

### Where it happens, and why there

Resolution lives inside `play()`, not at the call sites. That is why every `sfx::play()` in the
game became species-aware without changing: a call site names the **event**, and who is
speaking is context that belongs with the pet. `Pet::applyVoice()` pushes the current species
to the engine whenever it changes (hatch, evolve, boot, the species cheat), and everything
else follows.

Battle is the one place two creatures are on screen at once, so it is the one place that has
to say whose cry a sound is — `Params::voice` names the speaker explicitly there. `hit` and
`crit` are voiced as the creature being **hit**, not the one swinging: an impact sound is a
reaction, and the target is who the damage popup, the flash and the shake already belong to.
Left to the default, an enemy taking a hit would answer in the player's own voice, which reads
as the *player* being hit.

### Which base sounds are voiced

`eat`, `refuse`, `pet_happy`, `pet_sad`, `pet_sick`, `sleep`, `wake`, `hatch`, `evolve`, `hit`,
`crit`, `lose`. Not `feed`, `clean` or `heal` (those are the player's actions), not `win` or
`levelup` (the game congratulating you), and not the UI, minigame or music ids.

The **base** entry is what declares an id voiced — `<species>_pet_happy` is only ever reached
by resolving `pet_happy`. A mod that overrides a base sound and drops the flag turns voicing
off for that id, which is the right reading of having replaced the whole entry.

### What is bounded and what is not

**A roster's audio is not bounded.** Per-creature sound lives in per-creature folders, three of
which are resident at a time, so installing a thousand creatures with a thousand distinct
voices costs the same PSRAM as installing one.

What *is* bounded is the **global bank**: 256 entries (`MAX_SOUNDS`) holding device sounds, the
base game and shared voice families. None of those grow with the number of creatures installed,
which is what makes a fixed cap honest here. A `Sound` is 332 bytes, so the table is 83 KB, and
the three set slots are ~48 KB on top — all PSRAM, against ~7.3 MB free at boot.

The cap counts **unique ids, not files or bytes**. Overriding an existing sound reuses its slot,
so a mod that replaces every base-game sound costs zero new entries. Overflow is not fatal — the
entry is skipped with a `table full; dropped '<id>'` warning and the game runs on — but the scan
is weakest-root-first, so what gets lost is from the *strongest* roots: the SD card and the last
mod pack, i.e. exactly what someone just added. If a pack ever goes quiet in a way that makes no
sense, check the log for that line first.

One length limit is worth knowing: **`Sound::id` is 48 bytes.** Ids inside a creature folder are
short, but the rung-2 form is `<species>_<id>` and a creature id is itself up to 23 characters.
At the old 24 those composites truncated into each other — `metalgreymon_pet_sad` and
`metalgreymon_pet_sick` collapsing to one string is a sound bug with no symptom except the
wrong noise.

## Using it from code

```cpp
#include "engine/audio/sfx.hpp"

sfx::play(sfx::kHatch);                 // fire and forget
sfx::play(sfx::kHit, 0.45f);            // quieter

audio::Params p;                         // Params MODIFY the sound's own settings
p.pitch = 1.2f;                          // rather than replacing them
audio::Handle h = audio::play("eat", p);
audio::stop(h, 0.2f);                    // fade out over 200 ms

audio::VoiceProfile v{ c.id, c.voiceFamily, c.voicePitch };
p.voice = &v;                            // a voiced sound spoken by someone OTHER than the pet
audio::play(sfx::kHit, p);               // (battle only -- everywhere else the default is right)

audio::music(sfx::kMusicBattle);         // no-op if already playing that track
```

`Params` carries only what a call site legitimately knows — context. Bus, priority and base
volume live on the *sound*, because putting them at the call site would mean a mod could not
rebalance an effect without the code agreeing.

Ids are strings rather than an enum so that mod content can define and reference its own
sounds as first-class citizens. `sfx.hpp` gives the engine's own ids one spelling each, so a
typo is a link error instead of a sound that quietly never plays.

### Where the game fires them

Care sounds fire inside `sim/pet.cpp`, not in the scenes, because that is where the outcome is
known — only `feed()` can distinguish an accepted meal from a refused one, and only
`evolveTo()` can tell hatching from evolving. They are suppressed during `boot()`'s offline
replay, so catching up on eight hours away does not fire a stack of fanfares at power-on.

The cost of that placement is that a caller which *pre-gates* an action never reaches the code
that voices it — the home screen's Feed button checks `canEat()` and opens the picker, so a
refusal there was silent while the identical refusal inside the picker was not. `playRefusal()`
exists for that case: one spelling of "which no is this", asked of the state rather than of the
`refused_` wiggle flag (which `careBlocked()` has already set by then, and which therefore
cannot tell a frozen action from a sick creature). *Known rough edge*: the sim calling the
audio engine directly is why this needed a helper at all. The tidier shape is for the sim to
publish outcomes (it already does, via `checkAte`/`checkRefused` and the battle event queue)
and for scenes to voice them — worth doing when the care-feedback code is next opened.

Navigation sounds live in `App::setScene()`, which every screen already goes through — the
slide direction already encodes going deeper versus coming back. Which *track* plays is the
scene's own answer (`Scene::musicId()`, default `bgm_home`), so a new screen brings its music
with it instead of growing a second switch in `app.cpp`.

## Settings

**Settings → SOUND**: master, music and effects sliders plus a mute toggle, persisted in NVS
(`avol`, `amus`, `asfx`, `amut`). UI sounds ride the effects slider rather than getting a
fourth control — `Bus::Ui` resolves to the effects gain inside the mixer, so there is no second
value to keep in step. Music defaults to 40% — a looping chip tune on a device that sits on a
desk all day wears out its welcome, and a player who wants it can find the slider.

**The sliders are positions, not gains.** Perceived loudness goes roughly as the square of a
linear gain, so the engine squares a slider's position on the way into the mix; without that,
the top half of the travel barely changes what you hear. The curve lives in the mixer, not in
the settings scene, so a position restored from NVS or set by a mod means the same loudness as
one set by a finger. (Positions saved before this existed now sound quieter than they did —
they always *meant* what they now sound like.)

**Silence is free.** Muting or dragging a slider to zero parks the music track rather than
letting it decode, refill from the card and resample inaudibly; it resumes when the volume
does. Effects fired while inaudible — or while the screen is off, where the mixer is parked and
a sound would otherwise freeze at its first sample and burst out on wake — are dropped at
`play()`.

With the debug overlay on, that page also reports active voices, active streams, underruns and
peak mix time. **Underruns is the number that matters**: it counts blocks where a streaming
voice went dry, which is the one signal that separates "the card cannot keep up" from "that
file is broken". The overlay reads the peak without clearing it; the 5-second `AUDIO` log line
owns that window, so two readers cannot flatten each other's spikes.

## Costs

- Internal RAM: ~10 KB of mix scratch and DMA buffers, plus ~29 KB per live MP3 decoder and
  4 KB per live tracker.
- PSRAM: ring buffers (16 KB per stream), the sample cache (512 KB ceiling), the bank
  (~85 KB at the 256-entry cap), and a loaded module in full (50 KB – 1.5 MB).
- CPU: the voice mixer is ~2% of a core; MP3 decode is ~15%; a tracker is ~0.3% per channel
  on top of its one voice.
