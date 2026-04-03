/* cursor.c — 16x16 monochrome arrow cursor with save-under
 *
 * The cursor is defined by two bitmasks per row:
 *   cursor_mask[row]  — 1 = opaque pixel (draw something)
 *   cursor_color[row] — 1 = white fill, 0 = black outline (only where mask=1)
 *
 * Before drawing, the framebuffer pixels under the cursor are saved
 * to s_save[]. cursor_hide() restores them.
 */
#include "cursor.h"

/* Standard arrow cursor pointing upper-left.
 * Bit 15 = leftmost pixel, bit 0 = rightmost pixel of the 16-wide row. */
static const uint16_t cursor_mask[CURSOR_H] = {
    0xC000, /* 11.............. */
    0xF000, /* 1111............ */
    0xFC00, /* 111111.......... */
    0xFF00, /* 11111111........ */
    0xFFC0, /* 1111111111...... */
    0xFFF0, /* 111111111111.... */
    0xFFFC, /* 11111111111111.. */
    0xFFFF, /* 1111111111111111 */
    0xFFFC, /* 11111111111111.. */
    0xFFF0, /* 111111111111.... */
    0xFFC0, /* 1111111111...... */
    0xFE00, /* 1111111......... */
    0xF600, /* 1111.11......... */
    0xE300, /* 111...11........ */
    0xC300, /* 11....11........ */
    0x0100, /* .......1........ */
};

static const uint16_t cursor_color[CURSOR_H] = {
    0x0000, /* ................ */
    0x4000, /* .1.............. */
    0x7000, /* .111............ */
    0x7C00, /* .1111100........ */
    0x7F00, /* .1111111........ */
    0x7FC0, /* .111111111...... */
    0x7FF0, /* .11111111111.... */
    0x7FFC, /* .1111111111111.. */
    0x7FF0, /* .11111111111.... */
    0x7FC0, /* .111111111...... */
    0x7F00, /* .1111111........ */
    0x7C00, /* .11111.......... */
    0x6400, /* .11..1.......... */
    0x4100, /* .1.....1........ */
    0x0100, /* .......1........ */
    0x0000, /* ................ */
};

static surface_t *s_fb;
static uint32_t s_save[CURSOR_W * CURSOR_H];
static int s_x, s_y;
static int s_visible;

void cursor_init(surface_t *fb)
{
    s_fb = fb;
    s_visible = 0;
    s_x = 0;
    s_y = 0;
}

void cursor_hide(void)
{
    int row, col;
    if (!s_visible || !s_fb)
        return;
    for (row = 0; row < CURSOR_H; row++) {
        int py = s_y + row;
        if (py < 0 || py >= s_fb->h)
            continue;
        for (col = 0; col < CURSOR_W; col++) {
            int px = s_x + col;
            if (px < 0 || px >= s_fb->w)
                continue;
            s_fb->buf[py * s_fb->pitch + px] = s_save[row * CURSOR_W + col];
        }
    }
    s_visible = 0;
}

void cursor_show(int x, int y)
{
    int row, col;
    if (!s_fb)
        return;
    s_x = x;
    s_y = y;
    /* Save pixels under cursor */
    for (row = 0; row < CURSOR_H; row++) {
        int py = y + row;
        for (col = 0; col < CURSOR_W; col++) {
            int px = x + col;
            if (px >= 0 && px < s_fb->w && py >= 0 && py < s_fb->h)
                s_save[row * CURSOR_W + col] = s_fb->buf[py * s_fb->pitch + px];
            else
                s_save[row * CURSOR_W + col] = 0;
        }
    }
    /* Draw cursor sprite */
    for (row = 0; row < CURSOR_H; row++) {
        uint16_t m = cursor_mask[row];
        uint16_t c = cursor_color[row];
        int py = y + row;
        if (py < 0 || py >= s_fb->h)
            continue;
        for (col = 0; col < CURSOR_W; col++) {
            uint16_t bit = 0x8000 >> col;
            if (!(m & bit))
                continue;
            int px = x + col;
            if (px < 0 || px >= s_fb->w)
                continue;
            uint32_t color = (c & bit) ? 0x00FFFFFF : 0x00000000;
            s_fb->buf[py * s_fb->pitch + px] = color;
        }
    }
    s_visible = 1;
}

void cursor_move(int x, int y)
{
    if (!s_fb)
        return;
    cursor_hide();
    cursor_show(x, y);
}
