SD-CARD MOD PACK  --  "DMC V2 expansion"
=========================================

A test payload for the modding overlay. Everything here is loaded from the SD card at boot;
nothing needs reflashing, and deleting it puts the game back exactly as it was.


HOW TO INSTALL
--------------
Copy the CONTENTS of this folder (not the folder itself) to the ROOT of a FAT32 SD card, so
the card ends up looking like:

    <SD root>/creatures/punimon/creature.json
    <SD root>/creatures/punimon/sheet.png
    <SD root>/creatures/...            (9 creatures)
    <SD root>/foods/honey_drop/food.json
    <SD root>/foods/treat/food.json
    <SD root>/conversations/player/v2_mod_pack.json
    <SD root>/items/bounce_ball/item.json
    <SD root>/items/...                (3 items -- see note 6)

Insert the card BEFORE powering on -- the card is mounted once during boot.

You do not need to copy README.txt.


WHAT IT DEMONSTRATES
--------------------
1. ADDITIVE creatures. Nine DMC V2 digimon whose ids don't exist in flash, so they join the
   roster rather than replacing anything:
       punimon -> tsunomon -> gabumon | elecmon
               -> angemon | birdramon | kabuterimon | whamon -> metalmamemon
   Each ships a 4x4 sprite atlas (16 animation frames), same format as the base roster.

2. A CROSS-ROOT evolution edge. whamon can evolve into 'mamemon', which lives in FLASH.
   Evolution targets resolve against the MERGED registry, so an SD creature can evolve into a
   base one (and the reverse works too).

3. OVERRIDE by id. foods/treat/ has the same id as the base game's Sweet Treat, so the SD copy
   WINS: the Feed picker will show "Deluxe Treat" (purple, stronger, worse for health) where
   "Sweet Treat" used to be. Delete this one folder and the base treat returns.
   Foods are used for the override demo on purpose -- no save data refers to them, so it's
   entirely reversible.

4. ADDITIVE food. foods/honey_drop/ is simply a new option in the picker.

5. SD CONVERSATION PACKS. conversations/player/v2_mod_pack.json holds TWO conversations in one
   file (a JSON array). Packs work identically in flash and on SD; a mod may ship one pack for a
   whole campaign, or one file per conversation, whichever it prefers.

6. ITEMS -- INERT FOR NOW. items/ holds three fixtures for the economy system, one per item kind
   that mods will care about: a toy (bounce_ball, which carries personality drift the way a food
   does), a decor piece (paper_lantern, claiming the "feature" room slot) and a medicine
   (soothing_salve, with "treats": "injured" -- treatment is a priced item from E1 onward, so a
   mod can add its own remedy without the engine knowing its name).
   NOTHING READS THESE YET -- ItemRegistry arrives in economy phase E1, see
   docs/economy-and-inventory.md. They are here so the overlay can be tested the day it lands,
   and so the schema has a worked example. Until then they are simply ignored: an unknown folder
   costs nothing, in a loose tree or inside a .pak.

   foods/ already carries live economy data, though: honey_drop is "uncommon" at 35 Bits and the
   Deluxe Treat override is "rare" at 60, which makes both useful test cases for the rarity-based
   stock rotation in the shop.


EVOLUTION TIMING (IMPORTANT WHEN TESTING)
-----------------------------------------
The ladder was re-paced -- see docs/evolution-pacing.md. Champion now takes 2 WEEKS and Ultimate
6 WEEKS, where both used to be 3-4 days. This mod's creatures follow the same table, so
birdramon / kabuterimon / angemon / whamon sit at 2 weeks and metalmamemon at 6.

If you are testing evolution branches, do NOT wait for them. Menu > Settings > Cheats has a
species cycler and a force-evolve that walks the real edge list, so branch gates can be exercised
without waiting out two real weeks per stage.


WHAT TO LOOK FOR
----------------
Serial log at boot:
    CREA: sd: 'punimon' (Punimon) tier 1        <- nine of these, source tagged "sd"
    CREA: registry ready: 32 creatures          <- 23 flash + 9 SD
    FOOD: sd: 'treat' (Deluxe Treat) fills 16   <- the override
    FOOD: registry ready: 7 foods               <- 6 base + honey_drop (treat was replaced)
    CONV: scan: 5 files ...                     <- 4 flash + the SD pack

On the device:
    - Menu > Settings > Cheats has a species cycler; the nine new creatures appear in it, so
      you can morph into any of them immediately without waiting for evolutions.
    - The Feed picker shows Deluxe Treat (purple) and Honey Drop.
    - "mod/strange_taste" is repeatable and gated only on bond >= 500, so it should come round
      fairly soon; "mod/where_from" is a one-shot.


TO REMOVE
---------
Delete creatures/, foods/ and conversations/ from the card. If your pet is currently one of the
V2 species when you remove them, it falls back to the egg by design (Pet::boot logs
"saved creature '<id>' not in registry") -- so morph back to a base species first if you'd
rather not have that happen.


PACKING (OPTIONAL)
------------------
Instead of copying this tree loose, the whole mod can ship as ONE file:

    python tools/modpack.py pack sdcard_mod dmc_v2.pak

then copy dmc_v2.pak to <SD root>/mods/. Same content, same behaviour, but the device
reads one file instead of ~30 (much faster boot scans at large rosters) and installing
or removing the mod is a single copy/delete. Loose files still OVERRIDE packs on an id
clash, so both install styles can coexist. Details: docs/modpacks.md.


REGENERATING
------------
    python tools/digimon_import.py tools/rosters/dmc_v2_sd.json --out sdcard_mod/creatures
    python tools/conv_lint.py sdcard_mod        # validates mod + base content together
