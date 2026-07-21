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
                              "./game/engine/power.cpp"

                              # game: sim (simulation state + persistence)
                              "./game/sim/pet.cpp"
                              "./game/sim/creatures.cpp"
                              "./game/sim/save.cpp"

                              # game: battle (headless combat core)
                              "./game/battle/battle.cpp"

                              # game: scenes (screens, grouped by kind)
                              "./game/scenes/care/scene_home.cpp"
                              "./game/scenes/minigames/scene_run.cpp"
                              "./game/scenes/battle/scene_battle.cpp"
                              "./game/scenes/battle/scene_battle_draw.cpp"
                              "./game/scenes/battle/scene_battle_select.cpp"
                              "./game/scenes/menus/scene_menu.cpp"
                              "./game/scenes/menus/scene_settings.cpp"
                              "./game/scenes/menus/scene_timeset.cpp"
                              "./game/scenes/menus/scene_stats.cpp"
                              "./game/scenes/menus/scene_rename.cpp"

                              # vendor drivers (C)
                              "./drivers/Audio_Driver/PCM5101.c"
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

# Pack the base-game creature roster (flash_creatures/<id>/creature.json + sprite.png)
# into a read-only FAT image and flash it to the `creatures` partition. Mounted at
# /creatures on the device; SD-card creatures overlay it at runtime (SD wins on id clash).
fatfs_create_rawflash_image(creatures "${CMAKE_CURRENT_SOURCE_DIR}/../flash_creatures" FLASH_IN_PROJECT)
