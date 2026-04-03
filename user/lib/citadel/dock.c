/* dock.c -- macOS-style dock with frosted glass effect for Lumen compositor */
#include "dock.h"
#include <glyph.h>
#include <font.h>
#include <stdlib.h>
#include <string.h>

static int s_dock_x, s_dock_y, s_dock_w;
static int s_screen_w, s_screen_h;
static int s_hover_item = -1;

void
dock_init(int screen_w, int screen_h)
{
    s_screen_w = screen_w;
    s_screen_h = screen_h;

    s_dock_w = DOCK_PADDING_X * 2 +
               DOCK_ITEM_COUNT * DOCK_ICON_SIZE +
               (DOCK_ITEM_COUNT - 1) * DOCK_ICON_GAP;
    s_dock_x = (screen_w - s_dock_w) / 2;
    s_dock_y = screen_h - DOCK_HEIGHT - DOCK_BOTTOM_MARGIN;
}

/* Get the screen-space bounding box of a dock item's icon */
static void
dock_item_rect(int item, int *ix, int *iy)
{
    *ix = s_dock_x + DOCK_PADDING_X +
          item * (DOCK_ICON_SIZE + DOCK_ICON_GAP);
    *iy = s_dock_y + DOCK_PADDING_Y;
}

/* Check if (px,py) relative to dock origin is outside a rounded corner */
static int
outside_corner(int px, int py, int w, int h, int r)
{
    int cx, cy;

    /* Top-left */
    if (px < r && py < r) {
        cx = r; cy = r;
        int dx = cx - px, dy = cy - py;
        return dx * dx + dy * dy > r * r;
    }
    /* Top-right */
    if (px >= w - r && py < r) {
        cx = w - r - 1; cy = r;
        int dx = px - cx, dy = cy - py;
        return dx * dx + dy * dy > r * r;
    }
    /* Bottom-left */
    if (px < r && py >= h - r) {
        cx = r; cy = h - r - 1;
        int dx = cx - px, dy = py - cy;
        return dx * dx + dy * dy > r * r;
    }
    /* Bottom-right */
    if (px >= w - r && py >= h - r) {
        cx = w - r - 1; cy = h - r - 1;
        int dx = px - cx, dy = py - cy;
        return dx * dx + dy * dy > r * r;
    }
    return 0;
}

/* Draw gear icon for Settings */
static void
draw_icon_settings(surface_t *fb, int cx, int cy)
{
    /* Outer filled circle */
    draw_circle_filled(fb, cx, cy, 18, C_ACCENT);

    /* Gear teeth -- 4 small rectangles at cardinal directions */
    int tw = 8, th = 6;
    draw_fill_rect(fb, cx - tw / 2, cy - 22, tw, th, C_ACCENT);
    draw_fill_rect(fb, cx - tw / 2, cy + 16, tw, th, C_ACCENT);
    draw_fill_rect(fb, cx - 22, cy - th / 2, th, tw, C_ACCENT);
    draw_fill_rect(fb, cx + 16, cy - th / 2, th, tw, C_ACCENT);

    /* Diagonal teeth */
    int d = 14;
    draw_fill_rect(fb, cx + d - 3, cy - d - 3, 6, 6, C_ACCENT);
    draw_fill_rect(fb, cx - d - 3, cy - d - 3, 6, 6, C_ACCENT);
    draw_fill_rect(fb, cx + d - 3, cy + d - 3, 6, 6, C_ACCENT);
    draw_fill_rect(fb, cx - d - 3, cy + d - 3, 6, 6, C_ACCENT);

    /* Inner circle (use tint color for hollow center on glass) */
    draw_circle_filled(fb, cx, cy, 8, DOCK_GLASS_TINT);
}

/* Draw folder icon for File Explorer */
static void
draw_icon_folder(surface_t *fb, int ix, int iy)
{
    int fw = 40, fh = 28;
    int fx = ix + (DOCK_ICON_SIZE - fw) / 2;
    int fy = iy + (DOCK_ICON_SIZE - fh) / 2 + 2;

    draw_rounded_rect(fb, fx, fy - 6, 16, 8, 3, 0x00CC9944);
    draw_rounded_rect(fb, fx, fy, fw, fh, 4, 0x00CC9944);
    draw_fill_rect(fb, fx + 2, fy + 2, fw - 4, 3, 0x00BB8833);
}

/* Draw terminal icon */
static void
draw_icon_terminal(surface_t *fb, int ix, int iy)
{
    int tw = 42, th = 32;
    int tx = ix + (DOCK_ICON_SIZE - tw) / 2;
    int ty = iy + (DOCK_ICON_SIZE - th) / 2;

    draw_rounded_rect(fb, tx, ty, tw, th, 6, 0x001A1A2E);

    if (g_font_ui) {
        int tw2 = font_text_width(g_font_ui, 14, ">_");
        int text_x = tx + (tw - tw2) / 2;
        int text_y = ty + (th - font_height(g_font_ui, 14)) / 2;
        font_draw_text(fb, g_font_ui, 14, text_x, text_y, ">_", C_TERM_FG);
    } else {
        int text_x = tx + (tw - 3 * FONT_W) / 2;
        int text_y = ty + (th - FONT_H) / 2;
        draw_text(fb, text_x, text_y, ">_", C_TERM_FG, 0x001A1A2E);
    }
}

/* Draw widget/grid icon for Test Widgets */
static void
draw_icon_widgets(surface_t *fb, int ix, int iy)
{
    int bw = 38, bh = 30;
    int bx = ix + (DOCK_ICON_SIZE - bw) / 2;
    int by = iy + (DOCK_ICON_SIZE - bh) / 2;

    /* 2x2 grid of colored squares */
    int cell = 14, gap = 4;
    draw_rounded_rect(fb, bx, by, cell, cell, 3, 0x004488CC);              /* top-left: blue */
    draw_rounded_rect(fb, bx + cell + gap, by, cell, cell, 3, 0x0050FA7B); /* top-right: green */
    draw_rounded_rect(fb, bx, by + cell + gap, cell, cell, 3, 0x00FF7744); /* bot-left: orange */
    draw_rounded_rect(fb, bx + cell + gap, by + cell + gap, cell, cell, 3, 0x00CC66FF); /* bot-right: purple */
}

void
dock_draw(surface_t *fb, int screen_w, int screen_h)
{
    (void)screen_w;
    (void)screen_h;

    /*
     * Frosted glass effect:
     * 1. Blur the dock region of the backbuffer (already contains wallpaper + windows)
     * 2. Tint with semi-transparent dark color
     * 3. Mask corners to restore background outside rounded rect
     * 4. Draw icons on top
     */

    /* Step 1: Blur the dock region in-place */
    draw_box_blur(fb, s_dock_x, s_dock_y, s_dock_w, DOCK_HEIGHT,
                  DOCK_BLUR_RADIUS);

    /* Step 2: Semi-transparent tint over blurred region (rounded rect shape) */
    draw_blend_rect(fb, s_dock_x, s_dock_y, s_dock_w, DOCK_HEIGHT,
                    DOCK_GLASS_TINT, DOCK_GLASS_ALPHA);

    /* Step 3: Restore background pixels outside rounded corners.
     * We need the original unblurred background for corner pixels.
     * Since we already blurred in-place, we approximate by painting
     * a fully opaque background color over corner pixels. The
     * wallpaper/gradient behind the dock is close enough to C_BG1
     * at the bottom of the screen that this looks correct. For a
     * pixel-perfect result, we would need to save the background
     * before blurring, but for v0.1 the cost is not justified. */
    for (int py = 0; py < DOCK_HEIGHT; py++) {
        for (int px = 0; px < s_dock_w; px++) {
            if (outside_corner(px, py, s_dock_w, DOCK_HEIGHT, DOCK_CORNER_R)) {
                /* This pixel is outside the rounded rect -- we need to
                 * undo the blur+tint. Read the wallpaper pixel if available,
                 * otherwise use the default background. The compositor stores
                 * the wallpaper, but we do not have access to it here.
                 * Instead, re-read from the surface at a position outside
                 * the dock (one pixel above the dock at same x) as an
                 * approximation for the background. */
                int sx = s_dock_x + px;
                int sy = s_dock_y + py;
                if (sx >= 0 && sx < fb->w && sy >= 0 && sy < fb->h) {
                    /* Sample background from 1 pixel above the dock top.
                     * If that is also in the dock (shouldn't be), fall back
                     * to C_BG1. */
                    int bg_y = s_dock_y - 1;
                    uint32_t bg_px = C_BG1;
                    if (bg_y >= 0 && bg_y < fb->h)
                        bg_px = fb->buf[bg_y * fb->pitch + sx];
                    fb->buf[sy * fb->pitch + sx] = bg_px;
                }
            }
        }
    }

    /* Subtle border around the dock for definition */
    draw_blend_rect(fb, s_dock_x, s_dock_y, s_dock_w, 1,
                    0x00FFFFFF, 20);
    draw_blend_rect(fb, s_dock_x, s_dock_y + DOCK_HEIGHT - 1, s_dock_w, 1,
                    0x00000000, 40);

    /* Step 4: Draw dock items */
    for (int i = 0; i < DOCK_ITEM_COUNT; i++) {
        int ix, iy;
        dock_item_rect(i, &ix, &iy);

        /* Hover highlight -- semi-transparent lighter region */
        if (i == s_hover_item) {
            draw_blend_rect(fb, ix - 4, iy - 4,
                            DOCK_ICON_SIZE + 8, DOCK_ICON_SIZE + 8,
                            DOCK_HOVER_BG, DOCK_HOVER_ALPHA);
        }

        /* Draw the icon */
        switch (i) {
        case DOCK_ITEM_SETTINGS:
            draw_icon_settings(fb, ix + DOCK_ICON_SIZE / 2,
                               iy + DOCK_ICON_SIZE / 2);
            break;
        case DOCK_ITEM_FILES:
            draw_icon_folder(fb, ix, iy);
            break;
        case DOCK_ITEM_TERMINAL:
            draw_icon_terminal(fb, ix, iy);
            break;
        case DOCK_ITEM_WIDGETS:
            draw_icon_widgets(fb, ix, iy);
            break;
        }
    }
}

int
dock_hit_test(int mx, int my)
{
    if (mx < s_dock_x || mx >= s_dock_x + s_dock_w ||
        my < s_dock_y || my >= s_dock_y + DOCK_HEIGHT)
        return -1;

    for (int i = 0; i < DOCK_ITEM_COUNT; i++) {
        int ix, iy;
        dock_item_rect(i, &ix, &iy);
        if (mx >= ix && mx < ix + DOCK_ICON_SIZE &&
            my >= iy && my < iy + DOCK_ICON_SIZE)
            return i;
    }
    return -1;
}

void
dock_set_hover(int item)
{
    s_hover_item = item;
}
