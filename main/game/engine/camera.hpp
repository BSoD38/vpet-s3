#pragma once
#include <cmath>

// A 2D camera over a fixed world rectangle: a world-space view center plus a zoom factor.
// Scenes that opt in draw their WORLD content through the gfx *_world helpers (gfx.hpp),
// which consult one of these; HUD/UI keeps drawing in raw screen pixels as before, so a
// scene without a camera changes nothing.
//
// Deliberately pure math with no LovyanGFX (or any ESP-IDF) dependency: every mapping the
// renderer and the input layer need funnels through here, which is what makes the whole
// camera provable on the host (tools/camera_hosttest) before it ever touches the device.
//
// COORDINATE MODEL. World space is whatever the scene already draws in -- for Home that is
// the familiar 240x320 screen space, so at identity (zoom 1, centered) every mapping is a
// no-op and the *_world helpers fall through to the exact draw calls the scene made before
// the camera existed. zoom >= 1 shows a sub-rect of the world:
//
//     screen = (world - center) * zoom + view/2        (and the inverse for touch input)
//
// ROUNDING POLICY. Mappings return floats; rounding to pixels happens in exactly ONE way,
// projectRect(): both edges of a world rect are projected and floored INDEPENDENTLY, and
// the width is their difference. Two world rects sharing an edge therefore project to the
// same screen edge at any zoom -- no seams, no overlaps -- which is what lets the ground
// be drawn tile by tile. (Rounding each rect's width instead would let a row of tiles
// drift up to a pixel apart from its neighbours.)
//
// ROTATION is not implemented. If it is ever wanted: it slots in as an angle here (all
// mapping goes through sx/sy/wx/wy) and in the *_world draw helpers, whose LovyanGFX calls
// (pushRotateZoom / pushImageRotateZoom) already take an angle and are passed 0.
struct Camera2D {
    Camera2D(int viewW, int viewH, int worldW, int worldH)
        : viewW_((float)viewW), viewH_((float)viewH),
          worldW_((float)worldW), worldH_((float)worldH),
          cx_(worldW * 0.5f), cy_(worldH * 0.5f) {}

    float zoom() const { return zoom_; }

    // True when the mapping is a no-op (zoom exactly 1, view centered on the world center
    // it started at) -- the draw helpers use this to take the pre-camera code paths, so a
    // camera at rest costs nothing and renders pixel-identically to before.
    bool identity() const {
        return zoom_ == 1.0f && cx_ == viewW_ * 0.5f && cy_ == viewH_ * 0.5f;
    }

    // Raw zoom write; range policy belongs to the gesture (engine/pinch.hpp), not here.
    void setZoom(float z) { zoom_ = z > 0.001f ? z : 0.001f; }

    // Change zoom while keeping the world point currently under screen (sx, sy) exactly
    // where it is -- the anchor a pinch zooms around (its finger midpoint).
    void zoomAt(float sx, float sy, float newZoom) {
        float wx0 = wx(sx), wy0 = wy(sy);
        setZoom(newZoom);
        // Correct the center by however far the anchor drifted under the new zoom, read
        // back through wx/wy rather than re-deriving their formula here -- this is one of
        // the mappings the rounding/rotation note above promises funnels through them.
        cx_ += wx0 - wx(sx);
        cy_ += wy0 - wy(sy);
    }

    // Pan by a SCREEN-space delta (e.g. "the fingers moved 10px left"): world units scale
    // by 1/zoom, so content follows the finger 1:1 at any zoom.
    void panScreen(float dsx, float dsy) { cx_ += dsx / zoom_; cy_ += dsy / zoom_; }

    // Glide the view center toward a world-space target (a creature to keep on screen),
    // clamped to the world. Exponential smoothing: `rate` is per-second convergence (5 =
    // ~90% of the way in 0.45 s), frame-rate independent via the exp form. The clamp runs
    // after, so a target near a world edge parks the camera at the edge with the target
    // off-center rather than showing outside the world -- and at zoom 1 (world == view)
    // it re-centers exactly, so a follow camera at rest is still identity() and free.
    void follow(float twx, float twy, float dt, float rate) {
        float k = 1.0f - expf(-dt * rate);
        cx_ += (twx - cx_) * k;
        cy_ += (twy - cy_) * k;
        clampToWorld();
    }

    // Keep the visible rect inside the world. On an axis where the view is at least as
    // wide as the world, center it -- which at zoom 1 (world == view) restores the exact
    // starting center, so a snap back to 1x lands on identity() with no residue.
    void clampToWorld() {
        float hw = viewW_ * 0.5f / zoom_, hh = viewH_ * 0.5f / zoom_;
        cx_ = (hw * 2.0f >= worldW_) ? worldW_ * 0.5f
            : (cx_ < hw ? hw : (cx_ > worldW_ - hw ? worldW_ - hw : cx_));
        cy_ = (hh * 2.0f >= worldH_) ? worldH_ * 0.5f
            : (cy_ < hh ? hh : (cy_ > worldH_ - hh ? worldH_ - hh : cy_));
    }

    // world -> screen / screen -> world. Float in, float out: see the rounding policy above.
    float sx(float wxv) const { return (wxv - cx_) * zoom_ + viewW_ * 0.5f; }
    float sy(float wyv) const { return (wyv - cy_) * zoom_ + viewH_ * 0.5f; }
    float wx(float sxv) const { return cx_ + (sxv - viewW_ * 0.5f) / zoom_; }
    float wy(float syv) const { return cy_ + (syv - viewH_ * 0.5f) / zoom_; }
    float scale(float d) const { return d * zoom_; }   // world length -> screen length

    // The world rect currently on screen (for culling and for tiling's visible sub-range).
    float visX() const { return cx_ - viewW_ * 0.5f / zoom_; }
    float visY() const { return cy_ - viewH_ * 0.5f / zoom_; }
    float visW() const { return viewW_ / zoom_; }
    float visH() const { return viewH_ / zoom_; }

    // Cull test: does the world AABB [x,y,w,h] overlap the visible rect at all? The draw
    // helpers call this FIRST and skip the entire draw on a miss -- off-screen sprites
    // must cost nothing, not merely be clipped.
    bool visible(float x, float y, float w, float h) const {
        return x < visX() + visW() && x + w > visX() &&
               y < visY() + visH() && y + h > visY();
    }

    // THE int-rect projection (corner snap; see the rounding policy above). At identity
    // with integer inputs this returns them unchanged.
    void projectRect(float x, float y, float w, float h,
                     int& X, int& Y, int& W, int& H) const {
        X = (int)floorf(sx(x));
        Y = (int)floorf(sy(y));
        W = (int)floorf(sx(x + w)) - X;
        H = (int)floorf(sy(y + h)) - Y;
    }

private:
    float viewW_, viewH_;     // screen (panel) size
    float worldW_, worldH_;   // the fixed world rect the view is confined to
    float cx_, cy_;           // world point at the view center
    float zoom_ = 1.0f;       // >= 1 shows a sub-rect; the gesture owns the actual range
};
