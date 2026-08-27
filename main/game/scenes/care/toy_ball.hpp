#pragma once
#include <cmath>
#include "engine/gfx.hpp"
#include "engine/camera.hpp"
#include "esp_random.h"

// A ball you can actually play with: pick it up, throw it in any direction, and the creature
// runs it down and bats it away. The rest of the toys are a tap; this one is a game of catch.
//
// It is a 2-D PROJECTILE, not a ground roll. The first version tracked only x and drew a
// decorative hop on top, which meant the ball was welded to the ground line: you could not
// lift it, and a vertical flick produced no velocity at all because there was no axis for it
// to go into. Height is real here -- `y` is the ball's height above the ground, gravity pulls
// it down, and it bounces.
//
// Lives in its own header because SceneHome is already the biggest scene in the project and
// this is self-contained: position, velocity, and whether the creature is still interested.
// It knows nothing about Pet, Item or the economy -- the scene feeds it the creature's x and
// tells it when it is allowed to run.
//
// WORLD SPACE throughout. The ball is part of the room, so it pans and zooms with the pinch
// camera exactly like the grass; the scene maps the finger into world space before handing
// coordinates over, the same way the petting zone does.

constexpr float BALL_R        = 11.0f;
constexpr float BALL_GRAVITY  = 460.0f;   // px/s^2
constexpr float BALL_MAX_V    = 260.0f;   // clamp per axis, so a violent flick can't teleport it
constexpr float BALL_RESTITUTE= 0.52f;    // energy kept on a ground bounce
constexpr float BALL_WALL_KEEP= 0.60f;    // ...and on the edges of the ground
constexpr float BALL_ROLL_DRAG= 52.0f;    // rolling friction once it is down, px/s^2
constexpr float BALL_STOP_V   = 7.0f;     // below this along the ground it is at rest
constexpr float BALL_LAND_V   = 42.0f;    // below this vertically it stops bouncing
// How close the creature has to be to bat it, and how hard it hits.
constexpr float BALL_REACH    = 22.0f;
constexpr float BALL_HIT_H    = 26.0f;    // ...and how high it can still reach
constexpr float BALL_BAT_MIN  = 95.0f, BALL_BAT_RAND = 80.0f;
constexpr float BALL_BAT_LIFT = 150.0f;   // a bat pops it up as well as away
// The creature gives up if the ball has been still this long, so a forgotten ball does not
// hold it hostage at the far end of the room.
constexpr float BALL_BORED_S  = 6.0f;
// Drag-velocity smoothing (see drag()). ~70 ms of memory: long enough to survive the stalled
// final sample every touch panel produces on lift, short enough that a deliberate pause before
// letting go still reads as "put it down" rather than "throw it".
constexpr float DRAG_TAU      = 0.07f;
constexpr float DRAG_MIN_THROW= 18.0f;    // px/s below which a release is a placement

struct ToyBall {
    float x = 0.0f,  y = 0.0f;      // y = height ABOVE the ground; 0 = resting on it
    float vx = 0.0f, vy = 0.0f;
    float spin = 0.0f;              // roll angle, radians (see update)
    float idle = 0.0f;              // seconds at rest and untouched
    bool  held = false;             // finger currently carrying it
    bool  live = false;             // in play: the creature is interested
    float grabDx = 0.0f, grabDy = 0.0f;   // finger-to-ball offset, so it doesn't snap to the tip
    float lastX = 0.0f, lastY = 0.0f;
    float dragVx = 0.0f, dragVy = 0.0f;

    void place(float wx) { x = wx; y = 0; vx = vy = 0; spin = 0; idle = 0; held = false; live = false; }

    // Centre of the ball in world coordinates, given where the ground is.
    float cy(float groundY) const { return groundY - BALL_R - y; }

    bool contains(float wx, float wy, float groundY) const {
        const float dx = wx - x, dy = wy - cy(groundY);
        const float r = BALL_R + 10.0f;             // slack: it is a small target on a small screen
        return dx * dx + dy * dy <= r * r;
    }

    void grab(float wx, float wy, float groundY) {
        held = true; live = true;
        grabDx = x - wx;
        grabDy = y - (groundY - BALL_R - wy);
        lastX = x; lastY = y;
        dragVx = dragVy = 0.0f;
        idle = 0.0f;
    }

    // Carried by the finger in BOTH axes, so it can be lifted off the ground and thrown from
    // wherever it is let go.
    void drag(float wx, float wy, float groundY, float dt) {
        if (!held) return;
        const float nx = wx + grabDx;
        float ny = (groundY - BALL_R - wy) + grabDy;
        if (ny < 0.0f) ny = 0.0f;                   // cannot be pushed through the floor

        if (dt > 0.0f) {
            // Throw speed is a SMOOTHED estimate, not the last frame's delta.
            //
            // A touch panel almost always repeats its final coordinate on the way up, and a
            // finger decelerates as it leaves the glass, so the instantaneous delta at the
            // moment of release is very often exactly zero. Reading it directly made the ball
            // drop straight down however hard it was flicked -- there was no inertia to give
            // it. Averaging over ~DRAG_TAU keeps the motion from just before the lift, which
            // is the speed the player actually threw at, while a finger that genuinely comes
            // to rest before letting go still decays to zero and simply places the ball.
            const float k = 1.0f - expf(-dt / DRAG_TAU);
            dragVx += ((nx - lastX) / dt - dragVx) * k;
            dragVy += ((ny - lastY) / dt - dragVy) * k;
        }
        lastX = nx; lastY = ny;
        x = nx; y = ny;
        vx = vy = 0.0f;
    }

    void release() {
        if (!held) return;
        held = false;
        vx = clampV(dragVx);
        vy = clampV(dragVy);
        // Below this it was a placement, not a throw. Stops panel jitter from lobbing a ball
        // the player meant to set down.
        if (fabsf(vx) < DRAG_MIN_THROW && fabsf(vy) < DRAG_MIN_THROW) vx = vy = 0.0f;
        idle = 0.0f;
    }

    // `minX`/`maxX` are the creature's walkable span, so the ball can never come to rest
    // somewhere it cannot be fetched from.
    void update(float dt, float minX, float maxX) {
        if (held) return;

        // Rolling without slipping: one full turn per circumference travelled. Driven by vx
        // whether it is on the ground or in the air, so a thrown ball keeps the spin it left
        // the hand with instead of stopping dead in flight.
        spin += (vx / BALL_R) * dt;

        vy -= BALL_GRAVITY * dt;
        x  += vx * dt;
        y  += vy * dt;

        if (x < minX) { x = minX; vx = -vx * BALL_WALL_KEEP; }
        if (x > maxX) { x = maxX; vx = -vx * BALL_WALL_KEEP; }

        if (y <= 0.0f) {                            // hit the ground
            y = 0.0f;
            if (vy < -BALL_LAND_V) {
                vy = -vy * BALL_RESTITUTE;          // bounce
            } else {
                vy = 0.0f;                          // settled: roll it out
                const float drag = BALL_ROLL_DRAG * dt;
                if (fabsf(vx) <= drag) vx = 0.0f;
                else                   vx -= (vx > 0 ? drag : -drag);
                if (fabsf(vx) < BALL_STOP_V) vx = 0.0f;
            }
        }

        if (atRest()) idle += dt; else idle = 0.0f;
        if (idle > BALL_BORED_S) live = false;      // forgotten: the creature wanders off
    }

    bool atRest() const { return !held && y <= 0.01f && fabsf(vx) < BALL_STOP_V && fabsf(vy) < 1.0f; }

    // Can the creature reach it from where it is standing? Near enough along the ground AND
    // low enough to swat -- a ball sailing overhead is not battable, which is what makes a
    // high throw genuinely different from a roll.
    bool withinReach(float creatureX) const {
        return !held && fabsf(x - creatureX) <= BALL_REACH && y <= BALL_HIT_H;
    }

    // Knock it away. Biased AWAY from the creature and upward, so a bat always starts another
    // chase rather than trapping the ball under its feet.
    void batFrom(float creatureX) {
        const float dir = (x >= creatureX) ? 1.0f : -1.0f;
        vx = dir * (BALL_BAT_MIN + (float)(esp_random() % (uint32_t)BALL_BAT_RAND));
        vy = BALL_BAT_LIFT * 0.6f + (float)(esp_random() % 60u);
        idle = 0.0f;
        live = true;
    }

    void draw(const Camera2D& cam, float groundY, uint16_t color) const {
        // A flattened shadow on the ground, so height actually reads on a flat side view --
        // without it a thrown ball just looks like it moved up the screen.
        const float shade = 1.0f - (y / 180.0f);
        if (y > 2.0f) {
            const float sr = BALL_R * (shade < 0.35f ? 0.35f : shade);
            gfx_fill_rect_world(cam, x - sr, groundY - 3.0f, sr * 2.0f, 3.0f, rgb565(40, 60, 40));
        }
        const float ccy = cy(groundY);
        gfx_fill_circle_world(cam, x, ccy, BALL_R, color);
        // Two spots on opposite sides, orbiting with the roll angle. A plain sphere cannot
        // show rotation at all -- it needs a marking -- and two offset dots read as spin
        // while costing two more circles, with none of the angle-wrap traps a rotating arc
        // brings. They also make the direction of travel legible at a glance.
        const float ox = cosf(spin) * BALL_R * 0.45f;
        const float oy = sinf(spin) * BALL_R * 0.45f;
        const uint16_t spot = mix565(color, col::black, 0.45f);
        gfx_fill_circle_world(cam, x + ox, ccy + oy, BALL_R * 0.30f, spot);
        gfx_fill_circle_world(cam, x - ox, ccy - oy, BALL_R * 0.30f, spot);
        gfx_draw_circle_world(cam, x, ccy, BALL_R, col::black);
    }

private:
    static float clampV(float v) {
        if (v >  BALL_MAX_V) return  BALL_MAX_V;
        if (v < -BALL_MAX_V) return -BALL_MAX_V;
        return v;
    }
};
