/* window.c -- Glyph window: chrome rendering, dirty tracking, dispatch */
#include "glyph.h"
#include "font.h"
#include <stdlib.h>
#include <string.h>

#define TB_H  GLYPH_TITLEBAR_HEIGHT
#define BD_W  GLYPH_BORDER_WIDTH
#define SH_OFF GLYPH_SHADOW_OFFSET

/* Close button position within chrome (relative to surface origin) */
#define CLOSE_BTN_X  8
#define CLOSE_BTN_Y  (BD_W + 7)
#define CLOSE_BTN_SZ 14

/* Chrome colors -- frosted glass aesthetic */
#define C_CHROME_SHADOW  C_SHADOW
#define C_CHROME_BORDER  0x00505070
#define C_CHROME_TITLE   0x00222236
#define C_CHROME_UTITLE  0x00383848
#define C_CHROME_TITLE_T 0x00E8E8F0
#define C_CHROME_CLIENT  0x00F4F4F8
#define C_CHROME_RED     0x00FF5F57
#define C_CHROME_YELLOW  0x00FEBC2E
#define C_CHROME_GREEN   0x0028C840

/* Traffic-light button spacing */
#define BTN_RADIUS  7
#define BTN_SPACING 22
#define CORNER_R    10

/* Client area origin within the surface */
static int
client_ox(void)
{
    return BD_W;
}

static int
client_oy(void)
{
    return BD_W + TB_H;
}

glyph_window_t *
glyph_window_create(const char *title, int client_w, int client_h)
{
    glyph_window_t *win = calloc(1, sizeof(*win));
    if (!win)
        return NULL;

    win->client_w = client_w;
    win->client_h = client_h;
    win->surf_w = client_w + 2 * BD_W + SH_OFF;
    win->surf_h = client_h + TB_H + 2 * BD_W + SH_OFF;

    win->surface.buf = calloc((unsigned)(win->surf_w * win->surf_h), sizeof(uint32_t));
    if (!win->surface.buf) {
        free(win);
        return NULL;
    }
    win->surface.w = win->surf_w;
    win->surface.h = win->surf_h;
    win->surface.pitch = win->surf_w;

    if (title) {
        int len = 0;
        while (title[len] && len < 63) {
            win->title[len] = title[len];
            len++;
        }
        win->title[len] = '\0';
    }

    win->chrome_cache = calloc((unsigned)(win->surf_w * win->surf_h), sizeof(uint32_t));
    win->chrome_valid = 0;

    win->visible = 1;
    win->closeable = 1;
    win->frosted = 1;
    win->has_dirty = 1;
    win->dirty_rect.x = 0;
    win->dirty_rect.y = 0;
    win->dirty_rect.w = win->surf_w;
    win->dirty_rect.h = win->surf_h;

    return win;
}

void
glyph_window_destroy(glyph_window_t *win)
{
    if (!win)
        return;
    if (win->root)
        glyph_widget_destroy_tree(win->root);
    free(win->chrome_cache);
    free(win->surface.buf);
    free(win);
}

/* Check if pixel (px,py) is outside a rounded rectangle with corner radius r */
static int
outside_rounded(int px, int py, int w, int h, int r)
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

static void
render_chrome(glyph_window_t *win)
{
    surface_t *s = &win->surface;
    int total_w = win->client_w + 2 * BD_W;
    int total_h = win->client_h + TB_H + 2 * BD_W;
    int cx = client_ox();
    (void)client_oy();
    int focused = win->focused_window;

    /* Clear entire surface to key color (C_SHADOW) for frosted transparency */
    draw_fill_rect(s, 0, 0, win->surf_w, win->surf_h, C_CHROME_SHADOW);

    /* Draw the rounded window body.
     * Frosted windows: titlebar uses key color (C_SHADOW) so compositor's
     * blur+tint shows through. Non-frosted: solid titlebar. */
    uint32_t title_bg = win->frosted ? C_CHROME_SHADOW
                        : (focused ? C_CHROME_TITLE : C_CHROME_UTITLE);

    /* Titlebar area (rounded top corners) */
    for (int py = 0; py < TB_H + BD_W; py++) {
        for (int px = 0; px < total_w; px++) {
            if (!outside_rounded(px, py, total_w, total_h, CORNER_R))
                draw_px(s, px, py, title_bg);
        }
    }

    /* Client area (rounded bottom corners).
     * Frosted widget windows: use key color (C_SHADOW) so empty areas are
     * transparent in the compositor's keyed blit, showing frost through.
     * Terminal windows: use C_TERM_BG (also transparent via terminal keyed path).
     * Non-frosted: solid white client. */
    uint32_t client_bg = win->frosted ? C_CHROME_SHADOW
                         : C_CHROME_CLIENT;
    if (win->priv)
        client_bg = C_TERM_BG;

    for (int py = TB_H + BD_W; py < total_h; py++) {
        for (int px = 0; px < total_w; px++) {
            if (!outside_rounded(px, py, total_w, total_h, CORNER_R))
                draw_px(s, px, py, client_bg);
        }
    }

    /* Subtle border outline along the rounded rect (skip for frosted —
     * the frost tint provides enough definition) */
    if (!win->frosted) {
        for (int py = 0; py < total_h; py++) {
            for (int px = 0; px < total_w; px++) {
                if (outside_rounded(px, py, total_w, total_h, CORNER_R))
                    continue;
                if (px == 0 || py == 0 || px == total_w - 1 || py == total_h - 1 ||
                    outside_rounded(px - 1, py, total_w, total_h, CORNER_R) ||
                    outside_rounded(px + 1, py, total_w, total_h, CORNER_R) ||
                    outside_rounded(px, py - 1, total_w, total_h, CORNER_R) ||
                    outside_rounded(px, py + 1, total_w, total_h, CORNER_R))
                    draw_px(s, px, py, C_CHROME_BORDER);
            }
        }
    }

    /* Divider line between titlebar and client */
    for (int px = 1; px < total_w - 1; px++)
        draw_px(s, px, TB_H + BD_W - 1, C_CHROME_BORDER);

    /* Title text — centered */
    if (g_font_ui) {
        int tw = font_text_width(g_font_ui, 13, win->title);
        int tx = (total_w - tw) / 2;
        int ty = (TB_H + BD_W - font_height(g_font_ui, 13)) / 2;
        font_draw_text(s, g_font_ui, 13, tx, ty, win->title,
                       C_CHROME_TITLE_T);
    } else {
        int len = 0;
        const char *p = win->title;
        while (*p++) len++;
        int tx = (total_w - len * FONT_W) / 2;
        draw_text_t(s, tx, BD_W + 5, win->title, C_CHROME_TITLE_T);
    }

    /* Traffic-light circles */
    int btn_cy = (TB_H + BD_W) / 2;
    int btn_x = cx + CLOSE_BTN_X + BTN_RADIUS;

    draw_circle_filled(s, btn_x, btn_cy, BTN_RADIUS, C_CHROME_RED);
    draw_circle_filled(s, btn_x + BTN_SPACING, btn_cy, BTN_RADIUS, C_CHROME_YELLOW);
    draw_circle_filled(s, btn_x + BTN_SPACING * 2, btn_cy, BTN_RADIUS, C_CHROME_GREEN);

    /* X on close button when focused */
    if (focused)
        draw_text_t(s, btn_x - 3, btn_cy - FONT_H / 2, "x", 0x00401010);
}

void
glyph_window_render(glyph_window_t *win)
{
    if (!win || !win->has_dirty)
        return;

    /* Auto-invalidate chrome cache on focus change */
    if (win->chrome_valid && win->chrome_focused != win->focused_window)
        win->chrome_valid = 0;

    /* Render chrome from cache, or rebuild cache if invalid */
    if (win->chrome_cache && win->chrome_valid) {
        __builtin_memcpy(win->surface.buf, win->chrome_cache,
                         (unsigned long)(win->surf_w * win->surf_h) * 4);
    } else {
        render_chrome(win);
        if (win->chrome_cache) {
            __builtin_memcpy(win->chrome_cache, win->surface.buf,
                             (unsigned long)(win->surf_w * win->surf_h) * 4);
            win->chrome_valid = 1;
            win->chrome_focused = win->focused_window;
        }
    }

    /* Custom content renderer (terminal, info window) — called AFTER chrome
     * so it can draw into the client area without being overwritten. */
    if (win->on_render)
        win->on_render(win);

    /* Render widget tree into client area */
    if (win->root) {
        int ox = client_ox();
        int oy = client_oy();
        glyph_widget_draw_tree(win->root, &win->surface, ox, oy);
    }

    /* Focus ring omitted — widgets blend seamlessly with frosted glass */

    win->has_dirty = 0;
    win->dirty_rect.w = 0;
    win->dirty_rect.h = 0;
}

void
glyph_window_dispatch_mouse(glyph_window_t *win, int btn, int x, int y)
{
    if (!win)
        return;

    /* Check close button hit */
    if (win->closeable) {
        int cbx = BD_W + CLOSE_BTN_X;
        int cby = CLOSE_BTN_Y;
        if (x >= cbx && x < cbx + CLOSE_BTN_SZ &&
            y >= cby && y < cby + CLOSE_BTN_SZ) {
            if (win->on_close)
                win->on_close(win);
            return;
        }
    }

    /* Convert to client-area coordinates */
    int cx = x - client_ox();
    int cy = y - client_oy();

    /* Ignore clicks outside client area */
    if (cx < 0 || cy < 0 || cx >= win->client_w || cy >= win->client_h)
        return;

    /* Hit-test widget tree */
    if (win->root) {
        glyph_widget_t *hit = glyph_widget_hit_test(win->root, cx, cy);
        if (hit) {
            /* Focus on click */
            if (hit->focusable && hit != win->focused)
                glyph_window_set_focus(win, hit);

            /* Dispatch mouse to widget */
            if (hit->on_mouse) {
                /* Compute local coordinates within hit widget */
                int lx = cx, ly = cy;
                glyph_widget_t *p = hit;
                while (p) {
                    lx -= p->x;
                    ly -= p->y;
                    p = p->parent;
                }
                hit->on_mouse(hit, btn, lx, ly);
            }
        }
    }
}

void
glyph_window_dispatch_key(glyph_window_t *win, char key)
{
    if (!win)
        return;

    /* Custom key handler takes priority (e.g. terminal) */
    if (win->on_key) {
        win->on_key(win, key);
        return;
    }

    /* Tab cycles focus */
    if (key == '\t' && win->root) {
        glyph_widget_t *next = glyph_widget_focus_next(win->root, win->focused);
        if (next)
            glyph_window_set_focus(win, next);
        return;
    }

    /* Dispatch to focused widget */
    if (win->focused && win->focused->on_key)
        win->focused->on_key(win->focused, key);
}

int
glyph_window_get_dirty_rect(glyph_window_t *win, glyph_rect_t *out)
{
    if (!win || !win->has_dirty) {
        if (out)
            *out = (glyph_rect_t){0, 0, 0, 0};
        return 0;
    }
    if (out)
        *out = win->dirty_rect;
    return 1;
}

void
glyph_window_set_focus(glyph_window_t *win, glyph_widget_t *widget)
{
    if (!win)
        return;
    if (win->focused == widget)
        return;

    /* Mark old and new focused widgets dirty for redraw */
    if (win->focused)
        glyph_widget_mark_dirty(win->focused);
    win->focused = widget;
    if (widget)
        glyph_widget_mark_dirty(widget);
}

void
glyph_window_set_content(glyph_window_t *win, glyph_widget_t *root)
{
    if (!win)
        return;
    if (win->root)
        glyph_widget_destroy_tree(win->root);
    win->root = root;
    if (root)
        glyph_widget_set_window(root, win);
    glyph_window_mark_all_dirty(win);
}

void
glyph_window_mark_dirty_rect(glyph_window_t *win, glyph_rect_t r)
{
    if (!win)
        return;
    if (win->has_dirty) {
        win->dirty_rect = glyph_rect_union(win->dirty_rect, r);
    } else {
        win->dirty_rect = r;
        win->has_dirty = 1;
    }
}

void
glyph_window_mark_all_dirty(glyph_window_t *win)
{
    if (!win)
        return;
    win->dirty_rect.x = 0;
    win->dirty_rect.y = 0;
    win->dirty_rect.w = win->surf_w;
    win->dirty_rect.h = win->surf_h;
    win->has_dirty = 1;
}
