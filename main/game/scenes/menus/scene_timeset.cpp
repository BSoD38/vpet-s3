#include "scene_timeset.hpp"
#include "core/app.hpp"
#include "engine/gfx.hpp"
#include "ui/widgets.hpp"
#include "engine/drivers.hpp"   // datetime, datetime_t, PCF85063_Set_All, PCF85063_Read_Time

static const int ROW_N = 5;
static const char* ROW_LBL[ROW_N] = { "Year", "Month", "Day", "Hour", "Min" };

static const int ROW_Y0 = 58, ROW_DY = 36;
static const int LBL_X = 16, VAL_X = 140;
static const int MINUS_X = 92, PLUS_X = 192, STEP_W = 38, STEP_H = 32;
static Rect minus_btn(int i) { return { MINUS_X, ROW_Y0 + i * ROW_DY, STEP_W, STEP_H }; }
static Rect plus_btn (int i) { return { PLUS_X,  ROW_Y0 + i * ROW_DY, STEP_W, STEP_H }; }

static const Rect SET_BTN { 16,  250, 100, 46 };
static const Rect CAN_BTN { 126, 250, 98,  46 };

static const int YEAR_MIN = 2000, YEAR_MAX = 2069;   // driver stores year-1970 as BCD (<=99)

// press-and-hold auto-repeat
static const float HOLD_DELAY = 0.6f;   // hold this long before auto-repeat starts
static const float RPT_SLOW   = 0.2f;   // repeat interval right after the delay
static const float RPT_FAST   = 0.1f;  // repeat interval once it has ramped up
static const float RPT_RAMP   = 2.0f;    // seconds of repeating to reach full speed

static int days_in_month(int y, int m)
{
    static const int d[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    if (m == 2) { bool leap = (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0); return leap ? 29 : 28; }
    return d[(m - 1) % 12];
}

static int day_of_week(int y, int m, int d)   // Sakamoto's algorithm, 0=Sunday
{
    static const int t[12] = { 0,3,2,5,0,3,5,1,4,6,2,4 };
    if (m < 3) y -= 1;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

void SceneTimeSet::onEnter()
{
    // read into a LOCAL, not the shared `datetime` global (the core-0 driver task
    // owns writes to it; writing it here too would be a cross-core data race).
    datetime_t cur{};
    PCF85063_Read_Time(&cur);
    y_   = (cur.year < YEAR_MIN || cur.year > YEAR_MAX) ? 2026 : cur.year;
    mon_ = (cur.month < 1 || cur.month > 12) ? 1 : cur.month;
    day_ = (cur.day   < 1 || cur.day   > 31) ? 1 : cur.day;
    hour_ = cur.hour   > 23 ? 0 : cur.hour;
    min_  = cur.minute > 59 ? 0 : cur.minute;
    if (day_ > days_in_month(y_, mon_)) day_ = days_in_month(y_, mon_);
    down_ = false; heldRow_ = -1; heldTime_ = 0; repeatAcc_ = 0;
}

void SceneTimeSet::step(int row, int dir)
{
    switch (row) {
        case 0: y_ += dir;   if (y_ < YEAR_MIN) y_ = YEAR_MAX; if (y_ > YEAR_MAX) y_ = YEAR_MIN; break;
        case 1: mon_ += dir; if (mon_ < 1) mon_ = 12; if (mon_ > 12) mon_ = 1; break;
        case 2: { int dim = days_in_month(y_, mon_);
                  day_ += dir; if (day_ < 1) day_ = dim; if (day_ > dim) day_ = 1; } break;
        case 3: hour_ += dir; if (hour_ < 0) hour_ = 23; if (hour_ > 23) hour_ = 0; break;
        case 4: min_ += dir;  if (min_ < 0) min_ = 59;  if (min_ > 59) min_ = 0;  break;
    }
    int dim = days_in_month(y_, mon_);   // keep the day valid if month/year changed
    if (day_ > dim) day_ = dim;
}

void SceneTimeSet::update(float dt)
{
    // which stepper is the finger currently over?
    int row = -1, dir = 0;
    if (down_) {
        for (int i = 0; i < ROW_N; i++) {
            if      (minus_btn(i).contains(px_, py_)) { row = i; dir = -1; break; }
            else if (plus_btn(i).contains(px_, py_))  { row = i; dir = +1; break; }
        }
    }

    if (row < 0) {                       // not on a stepper -> idle
        heldRow_ = -1; heldTime_ = 0; repeatAcc_ = 0;
        return;
    }
    if (row != heldRow_ || dir != heldDir_) {   // just engaged a stepper -> one step now
        heldRow_ = row; heldDir_ = dir; heldTime_ = 0; repeatAcc_ = 0;
        step(row, dir);
        return;
    }

    // same stepper held: after a delay, repeat and accelerate
    heldTime_ += dt;
    if (heldTime_ >= HOLD_DELAY) {
        float t = (heldTime_ - HOLD_DELAY) / RPT_RAMP; if (t > 1.0f) t = 1.0f;
        float interval = RPT_SLOW + (RPT_FAST - RPT_SLOW) * t;
        repeatAcc_ += dt;
        while (repeatAcc_ >= interval) { repeatAcc_ -= interval; step(row, dir); }
    }
}

void SceneTimeSet::render()
{
    fb.fillScreen(col::panel);
    gfx_text(16, 16, 2, col::accent, "Set Time/Date");

    int vals[ROW_N] = { y_, mon_, day_, hour_, min_ };
    for (int i = 0; i < ROW_N; i++) {
        int ry = ROW_Y0 + i * ROW_DY;
        gfx_text(LBL_X, ry + 9, 2, col::white, "%s", ROW_LBL[i]);
        minus_btn(i).button("-", rgb565(60, 64, 84), col::white, 2, 5);
        if (i == 0) gfx_text(VAL_X, ry + 9, 2, col::good, "%d", vals[i]);
        else        gfx_text(VAL_X + 6, ry + 9, 2, col::good, "%02d", vals[i]);
        plus_btn(i).button("+", rgb565(60, 64, 84), col::white, 2, 5);
    }

    SET_BTN.button("Set",    col::good,          col::black, 2, 7);
    CAN_BTN.button("Cancel", rgb565(70, 74, 92), col::white, 2, 7);
}

void SceneTimeSet::onInput(const Input& in)
{
    // snapshot for the hold-repeat logic in update()
    down_ = in.down; px_ = in.x; py_ = in.y;

    if (!in.pressed) return;   // Set/Cancel are one-shot; steppers handled in update()

    if (SET_BTN.contains(in)) {
        datetime_t t{};
        t.year   = (uint16_t)y_;
        t.month  = (uint8_t)mon_;
        t.day    = (uint8_t)day_;
        t.hour   = (uint8_t)hour_;
        t.minute = (uint8_t)min_;
        t.second = 0;
        t.dotw   = (uint8_t)day_of_week(y_, mon_, day_);
        PCF85063_Set_All(t);
        // the core-0 driver task refreshes the `datetime` global within ~100ms;
        // don't write it from here (avoids the cross-core race).
        app().pet.markSaved();           // re-stamp lastUpdate to the new clock
        app().setScene(SceneId::Settings, Slide::Back);
        return;
    }
    if (CAN_BTN.contains(in)) {
        app().setScene(SceneId::Settings, Slide::Back);
        return;
    }
}
