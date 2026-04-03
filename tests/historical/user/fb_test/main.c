/* user/fb_test/main.c — Framebuffer GUI mockup demo
 *
 * Maps the linear framebuffer via sys_fb_map (513) and draws a
 * desktop-like GUI mockup: background gradient, window with title bar,
 * taskbar, and text. Uses Terminus 10x20 bitmap font.
 * Displays for 10 seconds, then clears and exits.
 */
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <sys/syscall.h>

typedef struct {
    uint64_t addr;
    uint32_t width, height, pitch, bpp;
} fb_info_t;

/* Terminus 10x20 font (SIL OFL 1.1) — 2 bytes per row, 20 rows per glyph */
#include "terminus20.h"

/* ── Drawing primitives ────────────────────────────────────────────── */

static uint32_t *s_fb;
static uint32_t s_pitch_px, s_w, s_h;

static inline void px(int x, int y, uint32_t c)
{
    if (x >= 0 && x < (int)s_w && y >= 0 && y < (int)s_h)
        s_fb[y * s_pitch_px + x] = c;
}

static void fill_rect(int x, int y, int w, int h, uint32_t c)
{
    int i, j;
    for (j = y; j < y + h && j < (int)s_h; j++)
        for (i = x; i < x + w && i < (int)s_w; i++)
            if (i >= 0 && j >= 0)
                s_fb[j * s_pitch_px + i] = c;
}

static void draw_rect(int x, int y, int w, int h, uint32_t c)
{
    int i;
    for (i = x; i < x + w; i++) { px(i, y, c); px(i, y + h - 1, c); }
    for (i = y; i < y + h; i++) { px(x, i, c); px(x + w - 1, i, c); }
}

static void draw_char(int x, int y, char ch, uint32_t fg, uint32_t bg)
{
    const uint8_t *glyph = &font_terminus[(unsigned char)ch * FONT_H * 2];
    int row, col;
    for (row = 0; row < FONT_H; row++) {
        uint16_t bits = ((uint16_t)glyph[row * 2] << 8) | glyph[row * 2 + 1];
        for (col = 0; col < FONT_W; col++) {
            uint32_t c = (bits & (0x8000 >> col)) ? fg : bg;
            px(x + col, y + row, c);
        }
    }
}

static void draw_text(int x, int y, const char *s, uint32_t fg, uint32_t bg)
{
    while (*s) {
        draw_char(x, y, *s, fg, bg);
        x += FONT_W;
        s++;
    }
}

static void draw_text_t(int x, int y, const char *s, uint32_t fg)
{
    while (*s) {
        const uint8_t *glyph = &font_terminus[(unsigned char)*s * FONT_H * 2];
        int row, col;
        for (row = 0; row < FONT_H; row++) {
            uint16_t bits = ((uint16_t)glyph[row * 2] << 8) | glyph[row * 2 + 1];
            for (col = 0; col < FONT_W; col++)
                if (bits & (0x8000 >> col))
                    px(x + col, y + row, fg);
        }
        x += FONT_W;
        s++;
    }
}

static void fill_gradient_v(int x, int y, int w, int h, uint32_t c1, uint32_t c2)
{
    int i, j;
    for (j = 0; j < h; j++) {
        int t = j * 255 / (h > 1 ? h - 1 : 1);
        uint32_t r = ((c1>>16&0xFF)*(255-t) + (c2>>16&0xFF)*t) / 255;
        uint32_t g = ((c1>>8&0xFF)*(255-t) + (c2>>8&0xFF)*t) / 255;
        uint32_t b = ((c1&0xFF)*(255-t) + (c2&0xFF)*t) / 255;
        uint32_t c = (r<<16)|(g<<8)|b;
        for (i = 0; i < w; i++)
            px(x + i, y + j, c);
    }
}

/* ── Colors ── */
#define C_BG1     0x001B2838
#define C_BG2     0x000D1B2A
#define C_BAR     0x001A1A2E
#define C_BAR_T   0x00C0C0D0
#define C_WIN     0x00FFFFFF
#define C_BORDER  0x00404060
#define C_TITLE   0x002D5F8A
#define C_TITLE_T 0x00FFFFFF
#define C_TEXT    0x00202030
#define C_RED     0x00E04040
#define C_GREEN   0x0040B040
#define C_YELLOW  0x00E0C040
#define C_BTN     0x003070A0
#define C_BTN_T   0x00FFFFFF
#define C_ACCENT  0x004488CC
#define C_SUBTLE  0x00808090
#define C_INFO_BG 0x00F0F4F8
#define C_INFO_BD 0x00C0C8D0
#define C_STATUS  0x00E8ECF0

int main(void)
{
    fb_info_t info;
    memset(&info, 0, sizeof(info));
    long ret = syscall(513, &info);
    if (ret < 0 || info.addr == 0) {
        printf("fb_test: no framebuffer (ret=%ld)\n", ret);
        return 1;
    }

    s_fb = (uint32_t *)(uintptr_t)info.addr;
    s_w = info.width;
    s_h = info.height;
    s_pitch_px = info.pitch / 4;

    /* No logging until the end — printk overwrites the framebuffer */

    /* ── Desktop background ── */
    fill_gradient_v(0, 0, (int)s_w, (int)s_h, C_BG1, C_BG2);

    /* ── Taskbar ── */
    int tb = (int)s_h - 36;
    fill_rect(0, tb, (int)s_w, 36, C_BAR);
    fill_rect(0, tb, (int)s_w, 1, C_ACCENT);
    fill_rect(6, tb+6, 90, 24, C_BTN);
    draw_rect(6, tb+6, 90, 24, C_ACCENT);
    draw_text(16, tb+8, "  Aegis", C_BTN_T, C_BTN);
    draw_text((int)s_w - 100, tb+8, "12:00 AM", C_BAR_T, C_BAR);

    /* ── Window ── */
    int ww = (int)s_w * 3 / 5;
    int wh = (int)s_h * 3 / 5;
    int wx = ((int)s_w - ww) / 2;
    int wy = ((int)s_h - 36 - wh) / 2;

    /* Shadow */
    fill_rect(wx+5, wy+5, ww, wh, 0x00080810);
    /* Body */
    fill_rect(wx, wy, ww, wh, C_WIN);
    draw_rect(wx, wy, ww, wh, C_BORDER);

    /* Title bar */
    fill_gradient_v(wx+1, wy+1, ww-2, 30, 0x003468A0, 0x002050780);
    draw_text(wx+12, wy+6, "System Information", C_TITLE_T, 0x003468A0);

    /* Window buttons */
    int bx = wx + ww - 80;
    int by = wy + 6;
    fill_rect(bx+52, by, 20, 18, C_RED);
    draw_text(bx+56, by-1, "x", C_WIN, C_RED);
    fill_rect(bx+28, by, 20, 18, C_GREEN);
    fill_rect(bx+4, by, 20, 18, C_YELLOW);

    /* ── Content area ── */
    int cx = wx + 20;
    int cy = wy + 42;
    int lh = FONT_H + 4;

    draw_text(cx, cy, "Welcome to Aegis", C_ACCENT, C_WIN);
    cy += lh + 6;

    /* Info box */
    int bw = ww - 40;
    int bh = 200;
    fill_rect(cx, cy, bw, bh, C_INFO_BG);
    draw_rect(cx, cy, bw, bh, C_INFO_BD);

    int ix = cx + 14;
    int iy = cy + 10;
    draw_text(ix, iy, "SYSTEM", C_ACCENT, C_INFO_BG);
    iy += lh + 2;
    fill_rect(ix, iy, bw - 28, 1, C_INFO_BD);
    iy += 6;

    draw_text(ix, iy, "Kernel:   Aegis 0.1 (x86_64)", C_TEXT, C_INFO_BG); iy += lh;
    draw_text(ix, iy, "Arch:     x86_64 (AMD64)", C_TEXT, C_INFO_BG); iy += lh;
    {
        char buf[80];
        snprintf(buf, sizeof(buf), "Display:  %ux%u @ 32bpp", s_w, s_h);
        draw_text(ix, iy, buf, C_TEXT, C_INFO_BG);
    }
    iy += lh;
    draw_text(ix, iy, "Security: Capability-based", C_TEXT, C_INFO_BG); iy += lh;
    draw_text(ix, iy, "Shell:    oksh (OpenBSD ksh)", C_TEXT, C_INFO_BG); iy += lh;
    draw_text(ix, iy, "Init:     Vigil supervisor", C_TEXT, C_INFO_BG); iy += lh;

    /* Status bar */
    int sy = wy + wh - 28;
    fill_rect(wx+1, sy, ww-2, 27, C_STATUS);
    fill_rect(wx+1, sy, ww-2, 1, C_INFO_BD);
    draw_text(wx+12, sy+5, "Framebuffer demo - Terminus 10x20 font", C_SUBTLE, C_STATUS);

    /* ── Desktop icons ── */
    int dx = 20, dy = 20;
    fill_rect(dx, dy, 56, 40, 0x00F0D860);
    fill_rect(dx, dy, 48, 8, 0x00E0C040);
    draw_rect(dx, dy+8, 56, 32, 0x00C0A030);
    draw_text_t(dx+3, dy+44, "Files", 0x00FFFFFF);

    dy += 80;
    fill_rect(dx, dy, 56, 40, 0x00202030);
    draw_rect(dx, dy, 56, 40, 0x00505070);
    draw_text(dx+3, dy+10, "$ _", 0x0040FF40, 0x00202030);
    draw_text_t(dx-5, dy+44, "Terminal", 0x00FFFFFF);

    sleep(10);

    /* Clear */
    { uint32_t i; for (i = 0; i < s_pitch_px * s_h; i++) s_fb[i] = 0; }

    printf("fb_test: GUI mockup complete (%ux%u)\n", s_w, s_h);
    return 0;
}
