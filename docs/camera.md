# Camera system — pan/zoom over a scene, pinch-to-zoom on Home

A reusable 2D camera for the engine ([`engine/camera.hpp`](../main/game/engine/camera.hpp)),
plus the first thing built on it: pinch-to-zoom on the Home scene (1x–4x, camera gliding
after the creature while zoomed). The point of the experiment is the seam it creates —
scenes can now be drawn in a coordinate space that isn't glued to the 240x320 panel, which
is what bigger scenes and scripted camera moves will need later.

## World space vs screen space

Every scene used to draw in raw panel pixels. With a camera there are two spaces:

- **World space** — where the scene's *content* lives. For Home the world is simply the
  classic 240x320 scene, so nothing about the sim or layout changed; zooming in shows a
  sub-rectangle of it.
- **Screen space** — the physical 240x320 panel. The HUD (top panel, action bar, menu
  button, debug overlay) never left it and is untouched by the camera.

The mapping is `screen = (world − center) · zoom + view/2`, owned by `Camera2D`: a world
view-center plus a zoom factor, with `clampToWorld()` keeping the visible rectangle inside
the world. The camera is **pure math** — no LovyanGFX, no ESP-IDF — which is what makes it
host-testable (below). Rotation is not implemented; if ever wanted, it slots into the same
four mapping functions and the draw helpers' rotate-zoom calls (which already take an
angle, currently 0).

## How a scene opts in

A scene keeps a `Camera2D` member and draws its **world** content through the `*_world`
variants in [`engine/gfx.hpp`](../main/game/engine/gfx.hpp) (`gfx_fill_rect_world`,
`gfx_blit_sprite_bottom_world`, `gfx_tile_region_world`, …), passing the same coordinates
it always used. HUD drawing stays on the plain calls. Touch input maps the other way:
screen → world via `cam.wx()/wy()` before hit-testing anything that lives in the world
(Home does this for the petting zone).

Three rules the helpers enforce:

- **Culling.** Every `*_world` call first tests its world bounding box against the visible
  rectangle and *skips the draw entirely* on a miss — an off-screen sprite costs nothing.
  The tile path goes further and only walks the tiles that are actually in view.
- **Identity is free.** At `identity()` (zoom exactly 1, centered — where a snap-back
  lands) every helper falls through to the pre-camera code path, so the scene renders
  pixel-identically to before the camera existed, for the cost of one comparison.
- **One rounding policy.** Mappings are float; rounding to pixels happens one way, in
  `projectRect()`: both edges of a world rect are projected and floored independently.
  Two rects sharing a world edge therefore share a screen edge at any zoom — which is why
  the tiled ground cannot open seams (each tile is rendered scaled to exactly its own
  snapped rectangle, plus a half-pixel of overscale so float rounding can't drop an edge
  column; the tiles are opaque, so the overscale is invisible).

Things that scale with the world on Home: ground tiles, poop, the creature (and its hop /
wiggle / footfall lift), the rub ring, hearts, sparkles. Things that stay at fixed screen
size but hang off the creature's *projected* position, nameplate-style: the SICK/Zzz text,
the speech bubble, the mood/attention/freeze badges — the text renderer only does integer
sizes, and a zoomed badge would just be blur.

## Zoomed sprites: nearest-neighbour, no cache, no AA

Camera zoom renders sprites through LovyanGFX's `pushRotateZoom` — the same call the
mirror blit already used, with the zoom factors doing double duty (magnitude = camera
zoom, x-sign = mirror). That is **nearest-neighbour on purpose**: it keeps pixel art
crisp and costs no resampling. It deliberately does *not* use the scaled-sprite LRU cache
(`gfx_blit_sprite_fit`): that cache is keyed on integer boxes and builds entries with an
expensive AA pass, so a continuously-varying pinch would rebuild it every frame.

## The pinch gesture, and the follow camera

[`engine/pinch.hpp`](../main/game/engine/pinch.hpp) turns the two-point touch data into
camera motion. It has two modes:

- **Free mode** (the default, for a future scene that wants it): zoom = finger-distance
  ratio anchored on the finger midpoint (the world point under the midpoint stays put),
  pan = midpoint movement, everything clamped to the world.
- **`zoomOnly` mode** (what Home uses): the pinch sets only the zoom *level*, because the
  framing belongs to `Camera2D::follow()` — the camera glides after the creature's body
  center every frame (exponential smoothing, ~90% of the way in 0.45 s, then clamped to
  the world, so a creature at the fence parks the camera at the fence). Anchoring or
  panning on the midpoint would fight the follow every frame; zooming about the camera's
  center *is* zooming on the creature, since the follow keeps it there. The follow target
  is the stable anchor (walk position + baseline) — deliberately not the footfall lift,
  poke hop, or refusal wiggle, so those animate the pet within the frame instead of
  shaking the whole world with it.

Either way, released below **1.15x it snaps to exactly 1x**, so the scene settles back
onto the free identity path; released above, the zoom persists (and survives a menu
round-trip, like the creature's position does).

The touch controllers report up to five points but no finger IDs, so the recognizer only
reads the two order-invariant quantities — distance and midpoint — and a slot swap between
frames cannot cause a jump. `engaged()` stays true from the moment two fingers are
accepted until the *last* finger leaves the screen: Home uses that one flag to stand down
rub/poke/taps, because to single-finger code a second finger looks like the point
teleporting (the rub tracker would happily bank that as petting distance). A pinch whose
very first finger lands on a button will still press it — that tap fires on the press edge,
before a second finger exists — accepted rather than adding a tap delay to every control.

The gesture is continuous, so it fires no UI sounds (the "every control voices itself"
rule is about discrete controls).

Input plumbing: `Input` now carries `points/x2/y2`
([`engine/input.hpp`](../main/game/engine/input.hpp)), read in the same single
`esp_lcd_touch_get_coordinates` call as before — both boards' controllers (CST328 and
CST3530) already read all points from the hardware, so **no driver changed**. The second
finger has its own, shorter presence bridge and produces no pressed/released edges;
gestures gate on the count.

Tuning lives as named constants at the top of `pinch.hpp` (`ZOOM_MAX`, `SNAP_BELOW`,
`MIN_PINCH_DIST` — the last rejects ghost pairs closer than a finger ever puts two real
touches).

## Host test

```
export PATH=/c/msys64/mingw64/bin:$PATH
g++ -std=gnu++20 -O2 -I main/game tools/camera_hosttest/harness.cpp -o harness && ./harness
```

Proves, in pure math: mapping round-trips, zoom-anchor invariance, clamp containment,
snap-to-1x landing on exact identity, `projectRect` identity exactness, the tile seam
property across 3001 zoom levels, culling, follow convergence / edge parking / zoom-1
identity, and scripted pinch sequences in both modes (ratio tracking, clamping, slot-swap
immunity, midpoint drag, zoomOnly leaving the center alone, the inert one-finger tail,
re-grab re-baselining, ghost-pair rejection). No shims needed — the camera and pinch
headers have no device dependencies at all.

## Performance notes

The one watched cost is the zoomed ground: at zoom > 1 every visible tile is an affine
blit instead of a memcpy-style push. The debug overlay (Settings → Debug info) shows FPS
plus a `cam z… p…` line for exactly this measurement. If it ever tanks, the pre-agreed
fallback is drawing the dirt region as a flat `gfx_fill_rect_world(col::ground)` while
zoomed and keeping only the single grass row tiled.
