#include "scene_rename.hpp"
#include "core/app.hpp"
#include "engine/gfx.hpp"
#include <cstdio>
#include <cstring>

// letter grid: rows 0..2 are 7 letters each; row 3 is V W X Y Z + space + backspace.
static const char* KROWS[3] = { "ABCDEFG", "HIJKLMN", "OPQRSTU" };
static const char* KROW3    = "VWXYZ";

static const int KX = 5, KTOP = 116, KW = 32, KH = 34, KGX = 1, KGY = 4;   // key grid
static const int FROW_Y = 272, FROW_H = 40, FB_W = 72;                     // function row
static const int FB0_X = 8, FB1_X = 84, FB2_X = 160;                       // case / cancel / done

// action codes returned by keyAt (letters + space are returned as their char, > 0)
static const int K_NONE = 0, K_BKSP = -1, K_CASE = -2, K_DONE = -3, K_CANCEL = -4;

static char cased(char up, bool lower) { return lower ? (char)(up - 'A' + 'a') : up; }

void SceneRename::onEnter()
{
    const char* cur = app().pet.nickname();
    len_ = 0;
    for (int i = 0; cur[i] && len_ < PET_NICK_MAX; i++) buf_[len_++] = cur[i];
    buf_[len_] = '\0';
    lower_ = false;
    t_ = 0.0f;
}

void SceneRename::update(float dt) { t_ += dt; }

int SceneRename::keyAt(int x, int y) const
{
    for (int r = 0; r < 4; r++) {
        int ky = KTOP + r * (KH + KGY);
        if (y < ky || y >= ky + KH) continue;
        for (int c = 0; c < 7; c++) {
            int kx = KX + c * (KW + KGX);
            if (x < kx || x >= kx + KW) continue;
            if (r < 3)      return cased(KROWS[r][c], lower_);
            if (c < 5)      return cased(KROW3[c], lower_);
            if (c == 5)     return ' ';        // space
            return K_BKSP;                     // backspace
        }
    }
    if (y >= FROW_Y && y < FROW_Y + FROW_H) {
        if (hit(x, y, FB0_X, FROW_Y, FB_W, FROW_H)) return K_CASE;
        if (hit(x, y, FB1_X, FROW_Y, FB_W, FROW_H)) return K_CANCEL;
        if (hit(x, y, FB2_X, FROW_Y, FB_W, FROW_H)) return K_DONE;
    }
    return K_NONE;
}

void SceneRename::onInput(const Input& in)
{
    if (!in.pressed) return;
    int k = keyAt(in.x, in.y);
    switch (k) {
        case K_NONE:   return;
        case K_CASE:   lower_ = !lower_; return;
        case K_BKSP:   if (len_ > 0) buf_[--len_] = '\0'; return;
        case K_CANCEL: app().setScene(SceneId::Stats, Slide::Back); return;
        case K_DONE:   app().pet.setNickname(buf_); app().setScene(SceneId::Stats, Slide::Back); return;
        default:       break;   // a letter or space
    }
    if (k == ' ' && len_ == 0) return;                       // no leading space
    if (len_ < PET_NICK_MAX) { buf_[len_++] = (char)k; buf_[len_] = '\0'; }
}

static void draw_key(int kx, int ky, const char* glyph)
{
    fb.fillRoundRect(kx, ky, KW, KH, 4, rgb565(52, 58, 80));
    int gw = (int)strlen(glyph) * 12;   // size-2 glyph is 12px wide per char
    gfx_text(kx + (KW - gw) / 2, ky + (KH - 16) / 2, 2, col::white, "%s", glyph);
}

void SceneRename::render()
{
    fb.fillScreen(col::panel);

    gfx_text(12, 8, 1, col::dim, "Name your %s", app().pet.speciesName());
    gfx_text(196, 8, 1, col::dim, "%d/%d", len_, PET_NICK_MAX);

    // text field + blinking cursor
    fb.fillRoundRect(12, 26, 216, 40, 6, col::black);
    fb.drawRoundRect(12, 26, 216, 40, 6, col::dim);
    gfx_text(22, 38, 2, col::white, "%s", buf_);
    if (((int)(t_ * 2.0f)) & 1) {
        int cx = 22 + len_ * 12;
        fb.fillRect(cx, 36, 3, 20, col::accent);
    }

    // letter grid
    char glyph[3];
    for (int r = 0; r < 4; r++) {
        int ky = KTOP + r * (KH + KGY);
        for (int c = 0; c < 7; c++) {
            int kx = KX + c * (KW + KGX);
            glyph[0] = glyph[1] = glyph[2] = '\0';
            if (r < 3)       glyph[0] = cased(KROWS[r][c], lower_);
            else if (c < 5)  glyph[0] = cased(KROW3[c], lower_);
            else if (c == 5) glyph[0] = '_';                 // space
            else { glyph[0] = '<'; glyph[1] = 'x'; }         // backspace
            draw_key(kx, ky, glyph);
        }
    }

    // function row: case toggle / cancel / done
    fb.fillRoundRect(FB0_X, FROW_Y, FB_W, FROW_H, 5, col::dim);
    gfx_text(FB0_X + (FB_W - 24) / 2, FROW_Y + 12, 2, col::black, lower_ ? "ab" : "AB");
    fb.fillRoundRect(FB1_X, FROW_Y, FB_W, FROW_H, 5, col::warn);
    gfx_text(FB1_X + (FB_W - 36) / 2, FROW_Y + 16, 1, col::white, "Cancel");
    fb.fillRoundRect(FB2_X, FROW_Y, FB_W, FROW_H, 5, col::good);
    gfx_text(FB2_X + (FB_W - 24) / 2, FROW_Y + 16, 1, col::black, "Done");
}
