/* window.c -- Glyph window: chrome rendering, dirty tracking, dispatch */
#include "glyph.h"
#include <stdlib.h>
#include <string.h>

#define TB_H  GLYPH_TITLEBAR_HEIGHT
#define BD_W  GLYPH_BORDER_WIDTH
#define SH_OFF GLYPH_SHADOW_OFFSET

/* Close button position within chrome (relative to surface origin) */
#define CLOSE_BTN_X  8
#define CLOSE_BTN_Y  (BD_W + 7)
#define CLOSE_BTN_SZ 14

/* Chrome colors -- same as old compositor */
#define C_CHROME_SHADOW  0x00080810
#define C_CHROME_BORDER  0x00404060
#define C_CHROME_TITLE1  0x003468A0
#define C_CHROME_TITLE2  0x00205078
#define C_CHROME_UTITLE1 0x00606070
#define C_CHROME_UTITLE2 0x00404050
#define C_CHROME_TITLE_T 0x00FFFFFF
#define C_CHROME_CLIENT  0x00FFFFFF
#define C_CHROME_RED     0x00E04040
#define C_CHROME_YELLOW  0x00E0C040
#define C_CHROME_GREEN   0x0040B040

/* Traffic-light button spacing */
#define BTN_RADIUS  7
#define BTN_SPACING 22

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

    win->visible = 1;
    win->closeable = 1;
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
    free(win->surface.buf);
    free(win);
}

static void
render_chrome(glyph_window_t *win)
{
    surface_t *s = &win->surface;
    int total_w = win->client_w + 2 * BD_W;
    int total_h = win->client_h + TB_H + 2 * BD_W;
    int cx = client_ox();
    int cy = client_oy();
    int focused = win->focused_window;

    /* Shadow — drawn to the right and below the window frame */
    draw_fill_rect(s, SH_OFF, SH_OFF, total_w, total_h, C_CHROME_SHADOW);

    /* Border — only the border strips, NOT the client area.
     * Top border (above titlebar) */
    draw_fill_rect(s, 0, 0, total_w, BD_W, C_CHROME_BORDER);
    /* Left border */
    draw_fill_rect(s, 0, BD_W, BD_W, TB_H + win->client_h + BD_W, C_CHROME_BORDER);
    /* Right border */
    draw_fill_rect(s, BD_W + win->client_w, BD_W, BD_W, TB_H + win->client_h + BD_W, C_CHROME_BORDER);
    /* Bottom border */
    draw_fill_rect(s, 0, BD_W + TB_H + win->client_h, total_w, BD_W, C_CHROME_BORDER);

    /* Titlebar gradient */
    uint32_t t1 = focused ? C_CHROME_TITLE1 : C_CHROME_UTITLE1;
    uint32_t t2 = focused ? C_CHROME_TITLE2 : C_CHROME_UTITLE2;
    draw_gradient_v(s, cx, BD_W, win->client_w, TB_H, t1, t2);

    /* Title text */
    draw_text_t(s, cx + 50, BD_W + 5, win->title, C_CHROME_TITLE_T);

    /* Traffic-light buttons */
    int btn_y = CLOSE_BTN_Y;
    int btn_x = cx + CLOSE_BTN_X;

    /* Close (red) */
    draw_fill_rect(s, btn_x, btn_y, BTN_RADIUS * 2, BTN_RADIUS * 2, C_CHROME_RED);
    if (focused)
        draw_text_t(s, btn_x + 3, btn_y - 2, "x", 0x00FFFFFF);

    /* Minimize (yellow) */
    draw_fill_rect(s, btn_x + BTN_SPACING, btn_y,
                   BTN_RADIUS * 2, BTN_RADIUS * 2, C_CHROME_YELLOW);

    /* Maximize (green) */
    draw_fill_rect(s, btn_x + BTN_SPACING * 2, btn_y,
                   BTN_RADIUS * 2, BTN_RADIUS * 2, C_CHROME_GREEN);

    /* Client area background — only for widget-tree windows.
     * Windows with priv (terminal, info) render their own client content. */
    if (!win->priv)
        draw_fill_rect(s, cx, cy, win->client_w, win->client_h, C_CHROME_CLIENT);
}

void
glyph_window_render(glyph_window_t *win)
{
    if (!win || !win->has_dirty)
        return;

    /* Render chrome (border, titlebar, buttons) */
    render_chrome(win);

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

    /* Draw focus ring on focused widget */
    if (win->focused && win->focused->visible) {
        glyph_widget_t *f = win->focused;
        /* Compute absolute position within surface */
        int fx = client_ox(), fy = client_oy();
        glyph_widget_t *p = f;
        while (p) {
            fx += p->x;
            fy += p->y;
            p = p->parent;
        }
        /* 2px blue focus ring */
        draw_rect(&win->surface, fx - 2, fy - 2, f->w + 4, f->h + 4, C_ACCENT);
        draw_rect(&win->surface, fx - 1, fy - 1, f->w + 2, f->h + 2, C_ACCENT);
    }

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
