/* taskbar.c -- Top bar and context menu for Lumen compositor
 *
 * This file replaces the old bottom taskbar with a macOS-style top bar.
 * The "Aegis" text on the left side is clickable and opens a context menu.
 */
#include "compositor.h"
#include <glyph.h>
#include <stdlib.h>
#include <string.h>

#define TOPBAR_BG       0x001A1A2E
#define TOPBAR_TEXT     0x00C0C0D0
#define AEGIS_AREA_W    80  /* clickable area width for "Aegis" text */

/* Top bar cache — static portion (background + accent dot + "Aegis" text)
 * only needs to be rendered once. Clock is drawn on top each time it changes. */
static uint32_t *s_topbar_cache;
static int s_topbar_cache_w;
static int s_topbar_cache_valid;
static char s_topbar_last_clock[8];

static void
topbar_render_static(int screen_w)
{
    if (!s_topbar_cache)
        return;

    surface_t cs;
    cs.buf = s_topbar_cache;
    cs.w = screen_w;
    cs.h = TOPBAR_HEIGHT;
    cs.pitch = screen_w;

    /* Top bar background */
    draw_fill_rect(&cs, 0, 0, screen_w, TOPBAR_HEIGHT, TOPBAR_BG);

    /* Accent dot before "Aegis" text */
    draw_circle_filled(&cs, 14, TOPBAR_HEIGHT / 2, 4, C_ACCENT);

    /* "Aegis" text */
    draw_text(&cs, 24, 4, "Aegis", 0x00FFFFFF, TOPBAR_BG);

    s_topbar_cache_w = screen_w;
    s_topbar_cache_valid = 1;
}

void
topbar_draw(surface_t *s, int screen_w, const char *clock_str)
{
    /* Allocate and render static cache on first call */
    if (!s_topbar_cache) {
        s_topbar_cache = malloc((size_t)screen_w * TOPBAR_HEIGHT * sizeof(uint32_t));
        s_topbar_cache_valid = 0;
        s_topbar_last_clock[0] = '\0';
    }

    if (!s_topbar_cache_valid || s_topbar_cache_w != screen_w)
        topbar_render_static(screen_w);

    /* Blit cached static portion to the target surface */
    if (s_topbar_cache) {
        for (int y = 0; y < TOPBAR_HEIGHT && y < s->h; y++)
            memcpy(&s->buf[y * s->pitch],
                   &s_topbar_cache[y * screen_w],
                   (size_t)screen_w * sizeof(uint32_t));
    }

    /* Draw clock on top — only the clock text changes per frame */
    if (clock_str && clock_str[0]) {
        int len = (int)strlen(clock_str);
        int cx = screen_w - len * FONT_W - 12;
        draw_text(s, cx, 4, clock_str, TOPBAR_TEXT, TOPBAR_BG);
    } else {
        draw_text(s, screen_w - 5 * FONT_W - 12, 4, "00:00", TOPBAR_TEXT, TOPBAR_BG);
    }
}

int
topbar_hit_aegis(int mx, int my, int screen_w)
{
    (void)screen_w;
    return mx >= 0 && mx < AEGIS_AREA_W &&
           my >= 0 && my < TOPBAR_HEIGHT;
}
