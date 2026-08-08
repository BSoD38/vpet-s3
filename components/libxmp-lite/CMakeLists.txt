# libxmp-lite as an ESP-IDF component.
#
# The source list below is taken verbatim from upstream's lite/src/Makefile and
# lite/src/loaders/Makefile -- those two files ARE the definition of "lite", so keeping this
# list in their order makes a version bump a diff rather than a puzzle. See README.md for how
# the tree was produced.
#
# Nothing here is modified from upstream. IT support is left ON: it costs a loader and gets
# Impulse Tracker modules for free (upstream's -DLIBXMP_CORE_DISABLE_IT would drop it).

idf_component_register(
    SRCS
        # src/ -- the player core
        "src/virtual.c"      "src/format.c"       "src/period.c"    "src/player.c"
        "src/read_event.c"   "src/misc.c"         "src/dataio.c"    "src/lfo.c"
        "src/scan.c"         "src/control.c"      "src/filter.c"    "src/effects.c"
        "src/flow.c"         "src/mixer.c"        "src/mix_all.c"   "src/load_helpers.c"
        "src/load.c"         "src/filetype.c"     "src/hio.c"       "src/smix.c"
        "src/memio.c"        "src/rng.c"          "src/win32.c"
        # src/loaders/ -- MOD, XM, S3M, IT
        "src/loaders/common.c"  "src/loaders/itsex.c"    "src/loaders/sample.c"
        "src/loaders/xm_load.c" "src/loaders/mod_load.c" "src/loaders/s3m_load.c"
        "src/loaders/it_load.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src" "src/loaders"
)

# LIBXMP_CORE_PLAYER is what selects the lite behaviour. LIBXMP_STATIC must be PUBLIC because
# xmp.h reads it to decide how to decorate the exported symbols.
target_compile_definitions(${COMPONENT_LIB} PRIVATE LIBXMP_CORE_PLAYER)
target_compile_definitions(${COMPONENT_LIB} PUBLIC  LIBXMP_STATIC)

# This project builds with -Werror, which third-party C written for a dozen other toolchains
# will trip. Demote errors to warnings for THIS component only: the warnings stay visible, an
# upstream bump cannot break our build over a style nit, and the project's own -Werror is
# untouched. Patching upstream to silence them is explicitly not the deal (see
# docs/sound-engine.md and the no-patching-vendored-libs rule).
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error)
