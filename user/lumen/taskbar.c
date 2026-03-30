/* taskbar.c -- Top bar and context menu for Lumen compositor
 *
 * This file replaces the old bottom taskbar with a macOS-style top bar.
 * The "Aegis" text on the left side is clickable and opens a context menu.
 */
#include "compositor.h"
#include "font.h"
#include <glyph.h>
#include <stdlib.h>
#include <string.h>

#define TOPBAR_BG       0x001A1A2E
#define TOPBAR_TEXT     0x00C0C0D0
#define AEGIS_AREA_W    80  /* clickable area width for "Aegis" text */

void
topbar_draw(surface_t *s, int screen_w, const char *clock_str)
{
    /* Frosted glass top bar: blur the top TOPBAR_HEIGHT pixels of the
     * backbuffer, apply a dark tint at 50% alpha, then draw text with
     * transparent background so the glass shows through. */
    draw_box_blur(s, 0, 0, screen_w, TOPBAR_HEIGHT, 8);
    draw_blend_rect(s, 0, 0, screen_w, TOPBAR_HEIGHT, TOPBAR_BG, 128);

    /* Subtle bottom border for definition */
    draw_blend_rect(s, 0, TOPBAR_HEIGHT - 1, screen_w, 1, 0x00FFFFFF, 20);

    /* Accent dot before "Aegis" text */
    draw_circle_filled(s, 14, TOPBAR_HEIGHT / 2, 4, C_ACCENT);

    /* "Aegis" text — transparent background */
    if (g_font_ui) {
        int ty = (TOPBAR_HEIGHT - font_height(g_font_ui, 14)) / 2;
        font_draw_text(s, g_font_ui, 14, 24, ty, "Aegis", 0x00FFFFFF);
    } else {
        draw_text_t(s, 24, 4, "Aegis", 0x00FFFFFF);
    }

    /* Clock text — transparent background */
    if (clock_str && clock_str[0]) {
        if (g_font_ui) {
            int cw = font_text_width(g_font_ui, 14, clock_str);
            int cx = screen_w - cw - 12;
            int ty = (TOPBAR_HEIGHT - font_height(g_font_ui, 14)) / 2;
            font_draw_text(s, g_font_ui, 14, cx, ty, clock_str, TOPBAR_TEXT);
        } else {
            int len = (int)strlen(clock_str);
            int cx = screen_w - len * FONT_W - 12;
            draw_text_t(s, cx, 4, clock_str, TOPBAR_TEXT);
        }
    } else {
        if (g_font_ui) {
            int cw = font_text_width(g_font_ui, 14, "00:00");
            int cx = screen_w - cw - 12;
            int ty = (TOPBAR_HEIGHT - font_height(g_font_ui, 14)) / 2;
            font_draw_text(s, g_font_ui, 14, cx, ty, "00:00", TOPBAR_TEXT);
        } else {
            draw_text_t(s, screen_w - 5 * FONT_W - 12, 4, "00:00", TOPBAR_TEXT);
        }
    }
}

int
topbar_hit_aegis(int mx, int my, int screen_w)
{
    (void)screen_w;
    return mx >= 0 && mx < AEGIS_AREA_W &&
           my >= 0 && my < TOPBAR_HEIGHT;
}
