/* dock.c -- macOS-style dock for Lumen compositor */
#include "dock.h"
#include <glyph.h>
#include <stdlib.h>
#include <string.h>

static int s_dock_x, s_dock_y, s_dock_w;
static int s_screen_w, s_screen_h;
static int s_hover_item = -1;

/* Cached dock surface — avoids redrawing circles/rounded-rects every frame */
static uint32_t *s_cache_buf;
static int s_cache_w, s_cache_h;
static int s_cache_valid;
static int s_cache_hover = -2; /* impossible initial value */

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

/* Draw gear icon for Settings */
static void
draw_icon_settings(surface_t *fb, int cx, int cy)
{
    /* Outer filled circle */
    draw_circle_filled(fb, cx, cy, 18, C_ACCENT);

    /* Gear teeth — 4 small rectangles at cardinal directions */
    int tw = 8, th = 6;
    /* Top */
    draw_fill_rect(fb, cx - tw / 2, cy - 22, tw, th, C_ACCENT);
    /* Bottom */
    draw_fill_rect(fb, cx - tw / 2, cy + 16, tw, th, C_ACCENT);
    /* Left */
    draw_fill_rect(fb, cx - 22, cy - th / 2, th, tw, C_ACCENT);
    /* Right */
    draw_fill_rect(fb, cx + 16, cy - th / 2, th, tw, C_ACCENT);

    /* Diagonal teeth (4 more at 45 degrees, approximated with small rects) */
    int d = 14;
    draw_fill_rect(fb, cx + d - 3, cy - d - 3, 6, 6, C_ACCENT);
    draw_fill_rect(fb, cx - d - 3, cy - d - 3, 6, 6, C_ACCENT);
    draw_fill_rect(fb, cx + d - 3, cy + d - 3, 6, 6, C_ACCENT);
    draw_fill_rect(fb, cx - d - 3, cy + d - 3, 6, 6, C_ACCENT);

    /* Inner circle (dock background to create hollow center) */
    draw_circle_filled(fb, cx, cy, 8, DOCK_BG);
}

/* Draw folder icon for File Explorer */
static void
draw_icon_folder(surface_t *fb, int ix, int iy)
{
    int fw = 40, fh = 28;
    int fx = ix + (DOCK_ICON_SIZE - fw) / 2;
    int fy = iy + (DOCK_ICON_SIZE - fh) / 2 + 2;

    /* Folder tab at top-left */
    draw_rounded_rect(fb, fx, fy - 6, 16, 8, 3, 0x00CC9944);

    /* Main folder body */
    draw_rounded_rect(fb, fx, fy, fw, fh, 4, 0x00CC9944);

    /* Slightly darker stripe near top to suggest depth */
    draw_fill_rect(fb, fx + 2, fy + 2, fw - 4, 3, 0x00BB8833);
}

/* Draw terminal icon */
static void
draw_icon_terminal(surface_t *fb, int ix, int iy)
{
    int tw = 42, th = 32;
    int tx = ix + (DOCK_ICON_SIZE - tw) / 2;
    int ty = iy + (DOCK_ICON_SIZE - th) / 2;

    /* Terminal background */
    draw_rounded_rect(fb, tx, ty, tw, th, 6, 0x001A1A2E);

    /* ">_" text centered */
    int text_x = tx + (tw - 3 * FONT_W) / 2;
    int text_y = ty + (th - FONT_H) / 2;
    draw_text(fb, text_x, text_y, ">_", C_TERM_FG, 0x001A1A2E);
}

static void
dock_render_to_cache(void)
{
    if (!s_cache_buf)
        return;

    /* Clear cache to transparent (desktop bg will show through via blit) */
    memset(s_cache_buf, 0, (size_t)s_cache_w * (size_t)s_cache_h * sizeof(uint32_t));

    /* Build a temporary surface for the cache, positioned at (0,0) */
    surface_t cs;
    cs.buf = s_cache_buf;
    cs.w = s_cache_w;
    cs.h = s_cache_h;
    cs.pitch = s_cache_w;

    /* Dock background with rounded corners — at (0,0) in cache coords */
    draw_rounded_rect(&cs, 0, 0, s_dock_w, DOCK_HEIGHT,
                      DOCK_CORNER_R, DOCK_BG);

    /* Draw each dock item (offset from s_dock_x to 0) */
    for (int i = 0; i < DOCK_ITEM_COUNT; i++) {
        int ix, iy;
        dock_item_rect(i, &ix, &iy);
        /* Convert from screen coords to cache-local coords */
        int lx = ix - s_dock_x;
        int ly = iy - s_dock_y;

        /* Hover highlight */
        if (i == s_hover_item) {
            draw_rounded_rect(&cs, lx - 4, ly - 4,
                              DOCK_ICON_SIZE + 8, DOCK_ICON_SIZE + 8,
                              8, DOCK_HOVER_BG);
        }

        /* Draw the icon */
        switch (i) {
        case DOCK_ITEM_SETTINGS:
            draw_icon_settings(&cs, lx + DOCK_ICON_SIZE / 2,
                               ly + DOCK_ICON_SIZE / 2);
            break;
        case DOCK_ITEM_FILES:
            draw_icon_folder(&cs, lx, ly);
            break;
        case DOCK_ITEM_TERMINAL:
            draw_icon_terminal(&cs, lx, ly);
            break;
        }
    }

    s_cache_valid = 1;
    s_cache_hover = s_hover_item;
}

void
dock_draw(surface_t *fb, int screen_w, int screen_h)
{
    (void)screen_w;
    (void)screen_h;

    /* Allocate cache on first call */
    if (!s_cache_buf) {
        s_cache_w = s_dock_w;
        s_cache_h = DOCK_HEIGHT;
        s_cache_buf = malloc((size_t)s_cache_w * (size_t)s_cache_h * sizeof(uint32_t));
        s_cache_valid = 0;
    }

    /* Re-render cache if hover state changed or cache is invalid */
    if (!s_cache_valid || s_cache_hover != s_hover_item)
        dock_render_to_cache();

    /* Blit cached dock to framebuffer */
    if (s_cache_buf)
        draw_blit(fb, s_dock_x, s_dock_y, s_cache_buf, s_cache_w, s_cache_h);
}

int
dock_hit_test(int mx, int my)
{
    /* Check if point is inside dock background */
    if (mx < s_dock_x || mx >= s_dock_x + s_dock_w ||
        my < s_dock_y || my >= s_dock_y + DOCK_HEIGHT)
        return -1;

    /* Check each item */
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
