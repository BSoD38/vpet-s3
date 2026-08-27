#pragma once
#include <cmath>
#include "input.hpp"
#include "camera.hpp"

// Two-finger pinch-zoom + drag-pan recognizer driving a Camera2D.
//
// Feed update() once per frame BEFORE the scene's own gesture logic, so the frame a second
// finger lands the scene can see engaged() and stand down its single-finger gestures --
// the touch layer has no finger IDs, so to one-finger code a second finger looks like the
// point teleporting, which Home's rub tracker would otherwise bank as petting distance.
//
// ORDER-INVARIANT BY CONSTRUCTION. The driver may report the two points in either order
// from frame to frame (no track IDs). The recognizer therefore reads exactly two derived
// quantities -- the distance between the points and their midpoint -- both of which are
// unchanged by swapping the points, so a slot swap cannot cause a jump.
//
// The gesture is continuous, so it fires no UI sounds (the "every control voices itself"
// rule is about discrete controls; a zoom that clicked every frame would be noise).
//
// States: IDLE (nothing) -> ACTIVE (two fingers tracked; zoom anchored on the midpoint
// and midpoint movement pans -- or zoom level only, in zoomOnly mode, when a follow
// camera owns the center) -> TAIL (fingers no longer both down, but the gesture isn't over
// until the screen is fully released -- the one finger that remains must stay inert, for
// the same no-IDs reason as above: nobody knows which finger it is). TAIL re-enters ACTIVE
// with fresh baselines if a second finger comes back.
struct PinchZoom {
    // Tuning.
    static constexpr float ZOOM_MIN       = 1.0f;
    static constexpr float ZOOM_MAX       = 4.0f;
    static constexpr float SNAP_BELOW     = 1.15f;  // release under this -> snap to exactly 1x
    static constexpr float MIN_PINCH_DIST = 24.0f;  // px between fingers; below this two
                                                    // "points" are more likely one finger +
                                                    // a ghost, so don't engage on them

    // When something else steers the view (a Camera2D::follow target, as on Home), the
    // pinch must only set the zoom LEVEL: anchoring on the finger midpoint or panning
    // with it would fight the follow every frame. Zoom then happens about the camera's
    // center -- which under a follow IS the followed target, so it zooms on the creature.
    bool zoomOnly = false;

    void update(const Input& in, Camera2D& cam)
    {
        bool two = in.points >= 2;
        float d = 0.0f, mx = 0.0f, my = 0.0f;
        if (two) {
            float dx = (float)(in.x2 - in.x), dy = (float)(in.y2 - in.y);
            d  = sqrtf(dx * dx + dy * dy);
            mx = (in.x + in.x2) * 0.5f;
            my = (in.y + in.y2) * 0.5f;
        }

        switch (state_) {
        case State::Idle:
        case State::Tail:
            if (two && d >= MIN_PINCH_DIST) {          // engage (or re-grab from the tail)
                d0_ = d;                               // fresh baselines either way: a
                z0_ = cam.zoom();                      // re-grab must scale from the zoom
                pmx_ = mx; pmy_ = my;                  // it finds, not the one it left
                state_ = State::Active;
            } else if (state_ == State::Tail && in.points == 0 && !in.down) {
                state_ = State::Idle;                  // fully released -> gesture over
            }
            break;

        case State::Active:
            if (two) {
                // Too close to be a real pinch (fingers touching, a ghost point, or the
                // release frame described below): HOLD -- leave the zoom and the midpoint
                // baseline exactly as they are. Clamping d and feeding it to the ratio
                // instead would read d << d0_ as "pinch all the way out" and slam the
                // camera to ZOOM_MIN in one frame. That is not hypothetical: when the
                // finger in driver slot 0 lifts first, the surviving finger is re-reported
                // in slot 0 while x2/y2 still hold its stale position for the frame the
                // input layer bridges (engine/input.cpp) -- so the pair momentarily
                // coincides, and half of all pinch releases would wipe the zoom to 1x.
                if (d < MIN_PINCH_DIST) break;
                float z = z0_ * (d / d0_);
                if (z < ZOOM_MIN) z = ZOOM_MIN;
                if (z > ZOOM_MAX) z = ZOOM_MAX;
                if (zoomOnly) {
                    cam.setZoom(z);                           // level only; the follower owns the center
                } else {
                    // Pan FIRST (at the pre-frame zoom), then zoom about the new midpoint.
                    // In this order the two compose exactly: the world point grabbed under
                    // the midpoint stays glued to it through a simultaneous pinch + drag.
                    cam.panScreen(pmx_ - mx, pmy_ - my);
                    cam.zoomAt(mx, my, z);
                }
                cam.clampToWorld();
                pmx_ = mx; pmy_ = my;
            } else {
                // A finger lifted. Near-1x is treated AS 1x: nobody releases at 1.08x on
                // purpose, and snapping the last fraction lands the camera on identity()
                // so the scene goes back to costing (and rendering) exactly what it did.
                if (cam.zoom() < SNAP_BELOW) { cam.setZoom(1.0f); cam.clampToWorld(); }
                state_ = State::Tail;
            }
            break;
        }
    }

    // True from engagement until EVERY finger has left the screen. Scenes gate their
    // single-touch gestures (and taps) on this rather than on in.points, so the leftover
    // finger of a released pinch can't poke, rub, or press anything on its way out.
    bool engaged() const { return state_ != State::Idle; }

    void reset() { state_ = State::Idle; }   // call from the scene's onEnter

private:
    enum class State { Idle, Active, Tail };
    State state_ = State::Idle;
    float d0_ = 1.0f;            // finger distance at engage (the zoom ratio's baseline)
    float z0_ = 1.0f;            // camera zoom at engage
    float pmx_ = 0.0f, pmy_ = 0.0f;   // previous midpoint (pan comes from its movement)
};
