#pragma once

// Watches for the SD card being yanked or inserted MID-SESSION.
//
// Everything SD-flavoured in this game is boot-time state: the mount itself, the
// creature/food/mod overlays, the update probe. A presence change can't be absorbed at
// runtime, so the game loop polls sdwatch_change() and halts on a full-screen restart
// prompt when it fires. The change is LATCHED and the watcher stops -- only the reboot
// resets it.
//
// Detection (the board has no card-detect line): a mounted card is health-checked with a
// cheap SEND_STATUS command every second; with no card mounted, a probe-only card init
// (no mount, no format) runs every couple of seconds until one answers. Both run on a
// low-priority core-0 task, so the probe's timeout cost never touches the game loop.

enum class SdChange { None = 0, Removed = 1, Inserted = 2 };

void     sdwatch_start();    // call once after SD_Init (captures the presence baseline)
SdChange sdwatch_change();   // latched change, polled by the game loop
