# Firmware & game-data updates (SD card)

Install a new firmware — and the base game data it depends on — **without a PC
connection**: package the build onto an SD card, then on the device run
**Settings → SYSTEM → System Update**.

## How to ship an update

1. Build as usual: `idf.py build`
2. Package onto the card (any FAT-formatted SD, files go at the **root**):

   ```
   python tools/make_update.py E:\
   ```

   This writes four files: `update.bin` (the app), `creatures.img` + `gamedata.img`
   (images of the FAT data partitions, straight from the build), and `update.json`
   (a manifest with the size + sha256 of each piece; written last, so its presence marks
   a complete package). Every copy is re-read and verified. Run the tool with no argument
   to just inspect the current build.
3. Insert the card **before powering the device on** (the card is mounted once at boot).
4. On the device: **Settings → SYSTEM → System Update** shows installed vs on-card
   versions and, per piece, whether it *will update* or is *up to date*; tap **INSTALL**.
   Progress takes on the order of half a minute; the device then reboots.

Only what differs is written: the device hashes each installed partition and compares it
against the manifest, so a code-only update doesn't rewrite 3 MB of unchanged data, a
data-only update skips the 30-second app install, and re-running an interrupted install
picks up where it left off. If nothing differs the screen just says **Up to date**.

The version shown comes from `version.txt` at the repo root (ESP-IDF embeds it into the
image as `esp_app_desc_t`) — bump it when shipping something meaningful, otherwise the
device will (harmlessly) report "this version is already installed".

## Why it's safe

- **A/B slots for the app.** The flash has two app partitions (`ota_0`/`ota_1`, 4 MB
  each). An update always streams into the slot **not currently running**; only after the
  full image passes ESP-IDF's validation (structure + embedded SHA-256) does `otadata`
  flip the boot choice. A yanked card, power cut, or corrupt file leaves the running game
  untouched.
- **Automatic rollback** (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`). The first boot of a
  new image runs in *pending-verify* state; the game marks itself valid only at the end of
  `App::init()` (`fw_confirm_running_image()`). If the new firmware crashes before that,
  the next reset boots the **previous** firmware instead of a brick.
- **Data partitions: verified writes + boot-time repair.** `creatures`/`gamedata` have no
  spare copy — they are rewritten in place, then verified by re-hashing the flash against
  the manifest (which also catches a corrupt file on the card). An NVS flag brackets the
  writes: if power dies mid-write, `fw_data_recovery()` redoes the unfinished partitions
  from the card at the next boot ("Repairing data" screen), *before* anything mounts that
  data. The install order — app first, data second, boot-flip **last** — means an
  interrupted install always boots the old app, which then heals the data.
- **Save data is never touched.** The pet lives in `nvs` at a fixed offset that no update
  path writes to. (A partition-table change is the one thing an update can NOT ship — that
  needs USB, which is why the slots are sized with headroom. For the same reason, a
  manifest entry for a data partition the device doesn't have is skipped, not an error.)

Bad candidates are rejected up front with a reason on screen: wrong chip, wrong project,
not an app image, doesn't fit the slot.

## Partition map (16 MB flash)

| name      | type/subtype | offset   | size  | notes                                   |
|-----------|--------------|----------|-------|-----------------------------------------|
| nvs       | data/nvs     | 0x9000   | 128K  | saves — MUST keep offset forever         |
| otadata   | data/ota     | 0x29000  | 8K    | which slot boots + rollback state        |
| ota_0     | app/ota_0    | 0x30000  | 4M    | app slot A (USB flashes land here)       |
| ota_1     | app/ota_1    | 0x430000 | 4M    | app slot B                               |
| creatures | data/fat     | 0x830000 | 1M    | base-game roster image                   |
| gamedata  | data/fat     | 0x930000 | 2M    | foods/natures/conversations image        |

Ends at 0xB30000 (~11.2 MB); ~4.8 MB free for future asset partitions.

## Developer notes

- **Serial flashing writes `ota_0`, but the bootloader boots whatever `otadata` says.**
  After the device has taken an SD update (which may have landed in `ota_1`), a plain
  `idf.py flash` puts your new build in `ota_0` while the device happily keeps booting the
  old one from `ota_1`. Dev flashes should therefore be:

  ```
  idf.py -p COM3 erase-otadata flash
  ```

  Erased `otadata` = "boot the first slot", which is exactly where the flash just landed.
  It's harmless when `otadata` was already clean, so it's safe to use always.
- `fw_probe()` (engine/fw_update.hpp) reads the fixed-offset headers of `update.bin` plus
  one hash pass over the affected partitions (well under a second) — fine for a scene's
  `onEnter()`.
- `fw_apply()` blocks the game loop and repaints via a progress callback; the scene
  forbids sleep (`allowsSleep() == false`) so the screen can't blank mid-install. Writing
  the data partitions while their read-only FAT mounts are live is safe *here* because
  nothing reads them during the install (conversation scanning pauses with sleep) and the
  device reboots immediately after — don't copy that pattern elsewhere.
- A card holding only `update.bin` (no manifest) still works as an app-only update — that
  is exactly what a 0.1.0 device sees, since data-update support itself arrives with
  0.1.1: the first SD update is app-only, everything after can carry data.
- **Data images are made deterministic on purpose**: fatfsgen embeds a random FAT volume
  id per run, which would make identical content hash as "changed" on every update.
  `tools/pin_fatfs_volid.py` (a POST_BUILD step in main/CMakeLists.txt) pins it, so the
  skip-unchanged comparison actually works. If updates ever start rewriting data that
  didn't change, check that this hook still runs.
- **SD presence is watched at runtime** (`engine/sdwatch`): yanking or inserting a card
  mid-session halts the game on a full-screen prompt (progress saved) with a restart
  button and a 30 s auto-restart — mounts and overlays are boot-time state, so a restart
  is the only honest way to absorb the change. Detection is by polling (no card-detect
  line): SEND_STATUS against a mounted card, a probe-only init (no mount, no format)
  when none is mounted.

## Future: Wi-Fi OTA

The expensive groundwork (A/B partitions, rollback, validation, confirm-on-boot) is what
this feature already built; Wi-Fi OTA is "the same `fw_apply` loop with a socket instead
of a `FILE*`" — e.g. fetch a manifest over HTTPS, stream the image with
`esp_http_client`, reuse the same progress UI. Note the common worry that OTA "halves
your flash" doesn't bite here: 16 MB comfortably holds both 4 MB slots **and** the data
partitions. An alternative — staging the download on the SD card and installing from
file — would also work with the existing code as-is, but there's no space pressure
pushing toward it.
