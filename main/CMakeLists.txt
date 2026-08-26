idf_component_register(
                         SRCS
                              "./main.cpp"

                              # game: core (composition root, loop, scene framework)
                              "./game/core/app.cpp"
                              "./game/core/game.cpp"

                              # game: engine (rendering, input, clock, power/sleep)
                              "./game/engine/display.cpp"
                              "./game/engine/gfx.cpp"
                              "./game/engine/input.cpp"
                              "./game/engine/clock.cpp"
                              "./game/engine/battery.cpp"
                              "./game/engine/power.cpp"
                              "./game/engine/fw_update.cpp"
                              "./game/engine/pakfs.cpp"
                              "./game/engine/sdwatch.cpp"

                              # game: engine/audio (4-voice mixer, decoders, chip synth, bank)
                              "./game/engine/audio/mixer.cpp"
                              "./game/engine/audio/decoder.cpp"
                              "./game/engine/audio/wav.cpp"
                              "./game/engine/audio/mp3.cpp"
                              "./game/engine/audio/synth.cpp"
                              "./game/engine/audio/tracker.cpp"
                              "./game/engine/audio/bank.cpp"

                              # game: sim (simulation state + persistence)
                              "./game/sim/pet.cpp"
                              "./game/sim/vitals.cpp"
                              "./game/sim/lineage.cpp"
                              "./game/sim/training.cpp"
                              "./game/sim/creatures.cpp"
                              "./game/sim/foods.cpp"
                              "./game/sim/items.cpp"
                              "./game/sim/economy.cpp"
                              "./game/sim/personality.cpp"
                              "./game/sim/conversation.cpp"
                              "./game/sim/gamedata.cpp"
                              "./game/sim/save.cpp"

                              # game: battle (headless combat core)
                              "./game/battle/battle.cpp"

                              # game: scenes (screens, grouped by kind)
                              "./game/scenes/care/scene_home.cpp"
                              "./game/scenes/care/scene_feed.cpp"
                              "./game/scenes/care/scene_conversation.cpp"
                              "./game/scenes/care/scene_death.cpp"
                              "./game/scenes/minigames/scene_run.cpp"
                              "./game/scenes/minigames/scene_mindmaze.cpp"
                              "./game/scenes/minigames/scene_smash.cpp"
                              "./game/scenes/minigames/scene_bulwark.cpp"
                              "./game/scenes/minigames/scene_stance.cpp"
                              "./game/scenes/battle/scene_battle.cpp"
                              "./game/scenes/battle/scene_battle_draw.cpp"
                              "./game/scenes/menus/scene_battle_select.cpp"
                              "./game/scenes/menus/scene_activities.cpp"
                              "./game/scenes/menus/scene_menu.cpp"
                              "./game/scenes/menus/scene_settings.cpp"
                              "./game/scenes/menus/scene_update.cpp"
                              "./game/scenes/menus/scene_cheats.cpp"
                              "./game/scenes/menus/scene_timeset.cpp"
                              "./game/scenes/menus/scene_about.cpp"
                              "./game/scenes/menus/scene_stats.cpp"
                              "./game/scenes/menus/scene_journal.cpp"
                              "./game/scenes/menus/scene_rename.cpp"
                              "./game/scenes/menus/scene_shop.cpp"

                              # vendor drivers (C). Audio_Driver is a pinout header only now --
                              # its player was replaced by game/engine/audio, so there is no .c
                              "./drivers/LCD_Driver/ST7789.c"
                              "./drivers/Touch_Driver/esp_lcd_touch/esp_lcd_touch.c"
                              "./drivers/Touch_Driver/CST328.c"
                              "./drivers/Touch_Driver/CST3530.c"
                              "./drivers/SD_Card/SD_MMC.c"
                              "./drivers/I2C_Driver/I2C_Driver.c"
                              "./drivers/PCF85063/PCF85063.c"
                              "./drivers/QMI8658/QMI8658.c"
                              "./drivers/BAT_Driver/BAT_Driver.c"
                              "./drivers/PWR_Key/PWR_Key.c"
                              "./drivers/Wireless/Wireless.c"

                         INCLUDE_DIRS
                              "."
                              # single game include root; game code uses folder-qualified
                              # includes (e.g. "engine/gfx.hpp", "sim/pet.hpp")
                              "./game"
                              # vendor driver headers (included flat, e.g. "ST7789.h")
                              "./drivers/Audio_Driver"
                              "./drivers/LCD_Driver"
                              "./drivers/Touch_Driver/esp_lcd_touch"
                              "./drivers/Touch_Driver"
                              "./drivers/SD_Card"
                              "./drivers/I2C_Driver"
                              "./drivers/PCF85063"
                              "./drivers/QMI8658"
                              "./drivers/BAT_Driver"
                              "./drivers/PWR_Key"
                              "./drivers/Wireless"
                       )

# Under -O2 the vendor C code trips GCC's string-op truncation analysis on some
# fixed-size name buffers (benign display-name truncation). Keep those as
# warnings rather than hard errors so the performance build succeeds.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=stringop-truncation
    -Wno-error=stringop-overflow
    -Wno-error=format-truncation
)

# --- Base-game data: one mod pack, not loose files ---------------------------------------
#
# All read-only base data (creatures, foods, natures, personalities, conversations) ships as a
# single base.pak inside the `gamedata` partition, which the device mounts as a pak root at
# boot. A pack holds its table of contents in PSRAM, so the whole data set costs one FAT open
# instead of a directory walk per file (~7-12 ms each here), and lazy sprite loads become a
# seek -- see docs/modpacks.md.
#
# flash_creatures/ and flash_gamedata/ REMAIN the tracked, diffable sources; authoring does not
# change. make_paks.py stages them into the pack's mod-root layout, which the two partition
# roots did not share (flash_creatures/<id>/ needs a `creatures/` prefix it has no reason to
# have on disk, while flash_gamedata/<system>/ is already at the right depth).
idf_build_get_property(fatfs_pin_python PYTHON)
set(BASE_PAK_DIR "${CMAKE_BINARY_DIR}/flash_data")
set(MAKE_PAKS    "${CMAKE_CURRENT_SOURCE_DIR}/../tools/make_paks.py")

# Once at CONFIGURE time, because fatfs_create_rawflash_image() below needs the image
# directory to exist and be populated...
execute_process(COMMAND ${fatfs_pin_python} ${MAKE_PAKS} ${BASE_PAK_DIR} base.pak
                RESULT_VARIABLE base_pak_result)
if(NOT base_pak_result EQUAL 0)
    message(FATAL_ERROR "tools/make_paks.py failed to build base.pak (${base_pak_result})")
endif()

# ...and again on every build, so editing a creature.json or a sprite actually reaches the
# device. Repacking is unconditional but cheap, and the pack is byte-deterministic (modpack.py
# sorts by path and stores no timestamps), so an unchanged source tree still yields an identical
# image -- which is what keeps the update system able to skip unchanged data partitions.
add_custom_target(base_pak ALL
    COMMAND ${fatfs_pin_python} ${MAKE_PAKS} ${BASE_PAK_DIR} base.pak
    COMMENT "Packing base game data -> flash_data/base.pak"
    VERBATIM)

fatfs_create_rawflash_image(gamedata "${BASE_PAK_DIR}" FLASH_IN_PROJECT)
add_dependencies(fatfs_gamedata_bin base_pak)   # repack before imaging, not after

# fatfsgen embeds a RANDOM volume id per run, making identical content hash differently;
# pin it so the SD-update system's "skip unchanged data partitions" comparison works
# (docs/firmware-updates.md). Runs after each (unconditional) image regeneration.
add_custom_command(TARGET fatfs_gamedata_bin POST_BUILD
    COMMAND ${fatfs_pin_python} ${CMAKE_CURRENT_SOURCE_DIR}/../tools/pin_fatfs_volid.py
            ${CMAKE_BINARY_DIR}/gamedata.bin
    COMMENT "Pinning FAT volume id of gamedata.bin")
