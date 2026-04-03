/* compositor.c -- Lumen window management and dirty-rect compositing */
#include "compositor.h"
#include "dock.h"
#include "cursor.h"
#include <glyph.h>
#include <font.h>
#include <stdlib.h>
#include <string.h>


/* ---- Helpers ---- */

/* Total window bounds on screen (including chrome + shadow) */
static glyph_rect_t
win_screen_rect(glyph_window_t *win)
{
    glyph_rect_t r;
    r.x = win->x;
    r.y = win->y;
    r.w = win->surf_w;
    r.h = win->surf_h;
    return r;
}

/* Check if screen point is inside a window's total area */
static int
point_in_window(glyph_window_t *win, int px, int py)
{
    return px >= win->x && px < win->x + win->surf_w &&
           py >= win->y && py < win->y + win->surf_h;
}

/* ---- Init ---- */

void
comp_init(compositor_t *c, uint32_t *fb, uint32_t *backbuf,
          int w, int h, int pitch)
{
    memset(c, 0, sizeof(*c));
    c->fb.buf = fb;
    c->fb.w = w;
    c->fb.h = h;
    c->fb.pitch = pitch;
    c->back.buf = backbuf;
    c->back.w = w;
    c->back.h = h;
    c->back.pitch = pitch;
    c->cursor_x = w / 2;
    c->cursor_y = h / 2;
    c->full_redraw = 1;
}

/* ---- Window management ---- */

void
comp_add_window(compositor_t *c, glyph_window_t *win)
{
    if (c->nwindows >= MAX_WINDOWS)
        return;
    c->windows[c->nwindows++] = win;
    c->focused = win;
    win->focused_window = 1;
    c->full_redraw = 1;
}

void
comp_remove_window(compositor_t *c, glyph_window_t *win)
{
    int idx = -1;
    for (int i = 0; i < c->nwindows; i++) {
        if (c->windows[i] == win) {
            idx = i;
            break;
        }
    }
    if (idx < 0)
        return;

    /* Mark the window's screen area as dirty before removal */
    comp_add_dirty(c, win_screen_rect(win));

    for (int i = idx; i < c->nwindows - 1; i++)
        c->windows[i] = c->windows[i + 1];
    c->nwindows--;

    if (c->focused == win)
        c->focused = c->nwindows > 0 ? c->windows[c->nwindows - 1] : NULL;

    /* Update focused_window flags */
    for (int i = 0; i < c->nwindows; i++)
        c->windows[i]->focused_window = (c->windows[i] == c->focused) ? 1 : 0;

    glyph_window_destroy(win);
    c->full_redraw = 1;
}

void
comp_raise_window(compositor_t *c, glyph_window_t *win)
{
    int idx = -1;
    for (int i = 0; i < c->nwindows; i++) {
        if (c->windows[i] == win) {
            idx = i;
            break;
        }
    }
    if (idx < 0 || idx == c->nwindows - 1)
        return;

    for (int i = idx; i < c->nwindows - 1; i++)
        c->windows[i] = c->windows[i + 1];
    c->windows[c->nwindows - 1] = win;
    c->full_redraw = 1;
}

glyph_window_t *
comp_window_at(compositor_t *c, int x, int y)
{
    for (int i = c->nwindows - 1; i >= 0; i--) {
        glyph_window_t *win = c->windows[i];
        if (!win->visible)
            continue;
        if (point_in_window(win, x, y))
            return win;
    }
    return NULL;
}

/* ---- Dirty rect management ---- */

void
comp_add_dirty(compositor_t *c, glyph_rect_t r)
{
    if (glyph_rect_empty(r))
        return;

    /* Clamp to screen */
    if (r.x < 0) { r.w += r.x; r.x = 0; }
    if (r.y < 0) { r.h += r.y; r.y = 0; }
    if (r.x + r.w > c->fb.w) r.w = c->fb.w - r.x;
    if (r.y + r.h > c->fb.h) r.h = c->fb.h - r.y;

    if (r.w <= 0 || r.h <= 0)
        return;

    if (c->ndirty < MAX_DIRTY_RECTS) {
        c->dirty_rects[c->ndirty++] = r;
    } else {
        /* Overflow: union into last rect */
        c->dirty_rects[MAX_DIRTY_RECTS - 1] =
            glyph_rect_union(c->dirty_rects[MAX_DIRTY_RECTS - 1], r);
    }
}

/* ---- Desktop selection box ---- */

static void
draw_selection_box(surface_t *s, compositor_t *c)
{
    if (!c->selecting) return;
    int x0 = c->sel_x0 < c->sel_x1 ? c->sel_x0 : c->sel_x1;
    int y0 = c->sel_y0 < c->sel_y1 ? c->sel_y0 : c->sel_y1;
    int w = abs(c->sel_x1 - c->sel_x0);
    int h = abs(c->sel_y1 - c->sel_y0);
    if (w < 2 || h < 2) return;
    /* Translucent blue fill */
    draw_blend_rect(s, x0, y0, w, h, 0x003060A0, 40);
    /* Border — slightly more opaque */
    draw_blend_rect(s, x0, y0, w, 1, 0x004488CC, 120);
    draw_blend_rect(s, x0, y0 + h - 1, w, 1, 0x004488CC, 120);
    draw_blend_rect(s, x0, y0, 1, h, 0x004488CC, 120);
    draw_blend_rect(s, x0 + w - 1, y0, 1, h, 0x004488CC, 120);
}

/* ---- Composite ---- */

/* Blit modes: 0 = full frost (blur+tint+keyed), 1 = fast frost (tint+keyed,
 * no blur — used during drag for non-dragged windows), 2 = opaque (dragged window) */
#define BLIT_FROST      0
#define BLIT_FAST_FROST  1
#define BLIT_OPAQUE      2

static void
blit_window_to_back(surface_t *back, glyph_window_t *win, int mode)
{
    if (win->frosted && win->chromeless && mode != BLIT_OPAQUE) {
        /* Chromeless frosted window (dropdown terminal) */
        if (mode == BLIT_FROST)
            draw_box_blur(back, win->x, win->y, win->surf_w, win->surf_h, 10);
        draw_blend_rect(back, win->x, win->y, win->surf_w, win->surf_h,
                        C_TERM_BG, 128);
        draw_blit_keyed(back, win->x, win->y, win->surface.buf,
                        win->surf_w, win->surf_h, C_TERM_BG);
    } else if (win->frosted && mode != BLIT_OPAQUE) {
        int bd = GLYPH_BORDER_WIDTH;
        int tb = GLYPH_TITLEBAR_HEIGHT;
        int total_w = win->client_w + 2 * bd;
        int total_h = win->client_h + tb + 2 * bd;

        /* 1. Blur the entire window footprint (skip during fast frost) */
        if (mode == BLIT_FROST)
            draw_box_blur(back, win->x, win->y, total_w, total_h, 10);

        /* 2. Dark tint on titlebar region */
        draw_blend_rect(back, win->x, win->y, total_w, tb + bd,
                        0x00101020, 160);

        /* 3. Tint on client region — dark for terminals, light for widgets */
        if (win->priv) {
            /* Terminal: dark frosted glass matching terminal bg */
            draw_blend_rect(back, win->x + bd, win->y + tb + bd,
                            win->client_w, win->client_h,
                            0x000A0A14, 160);
        } else {
            /* Widget window: dark translucent glass (not white) */
            draw_blend_rect(back, win->x + bd, win->y + tb + bd,
                            win->client_w, win->client_h,
                            0x00181828, 150);
        }

        /* 4. Subtle border around entire window */
        draw_blend_rect(back, win->x, win->y, total_w, 1, 0x00FFFFFF, 30);
        draw_blend_rect(back, win->x, win->y + total_h - 1, total_w, 1, 0x00000000, 40);
        draw_blend_rect(back, win->x, win->y, 1, total_h, 0x00FFFFFF, 20);
        draw_blend_rect(back, win->x + total_w - 1, win->y, 1, total_h, 0x00000000, 30);

        /* 5. Title text — drawn directly on frosted backbuffer (no halo) */
        int title_cy = win->y + (tb + bd) / 2;
        if (g_font_ui) {
            int tw = font_text_width(g_font_ui, 13, win->title);
            int tx = win->x + (total_w - tw) / 2;
            int ty = title_cy - font_height(g_font_ui, 13) / 2;
            font_draw_text(back, g_font_ui, 13, tx, ty, win->title, 0x00FFFFFF);
        } else {
            int len = 0;
            const char *p = win->title;
            while (*p++) len++;
            int tx = win->x + (total_w - len * FONT_W) / 2;
            draw_text_t(back, tx, win->y + bd + 5, win->title, 0x00FFFFFF);
        }

        /* 6. Traffic-light circles — drawn directly on backbuffer */
        int btn_cy = title_cy;
        int btn_x = win->x + bd + 8 + 7;
        draw_circle_filled(back, btn_x, btn_cy, 7, 0x00FF5F57);
        draw_circle_filled(back, btn_x + 22, btn_cy, 7, 0x00FEBC2E);
        draw_circle_filled(back, btn_x + 44, btn_cy, 7, 0x0028C840);

        /* 7. Blit client area content with color keying (transparent pixels) */
        {
            int cx = bd, cy = bd + tb;
            int dx = win->x + cx, dy = win->y + cy;
            int cw = win->client_w, ch2 = win->client_h;
            /* Clamp to backbuffer bounds once */
            int sx0 = 0, sy0 = 0;
            if (dx < 0) { sx0 = -dx; cw += dx; dx = 0; }
            if (dy < 0) { sy0 = -dy; ch2 += dy; dy = 0; }
            if (dx + cw > back->w) cw = back->w - dx;
            if (dy + ch2 > back->h) ch2 = back->h - dy;
            uint32_t key = win->priv ? C_TERM_BG : C_SHADOW;
            for (int row = 0; row < ch2; row++) {
                uint32_t *src_row = &win->surface.buf[(cy + sy0 + row) * win->surface.pitch + cx + sx0];
                uint32_t *dst_row = &back->buf[(dy + row) * back->pitch + dx];
                for (int col = 0; col < cw; col++) {
                    if (src_row[col] != key)
                        dst_row[col] = src_row[col];
                }
            }
        }
    } else {
        /* Normal opaque blit */
        draw_blit(back, win->x, win->y, win->surface.buf,
                  win->surf_w, win->surf_h);
    }
}

static void
partial_flip(surface_t *fb, surface_t *back, glyph_rect_t r)
{
    /* Copy only the dirty rect from backbuffer to framebuffer */
    for (int y = r.y; y < r.y + r.h && y < fb->h; y++) {
        if (y < 0) continue;
        int x0 = r.x < 0 ? 0 : r.x;
        int x1 = r.x + r.w;
        if (x1 > fb->w) x1 = fb->w;
        int count = x1 - x0;
        if (count <= 0) continue;
        memcpy(&fb->buf[y * fb->pitch + x0],
               &back->buf[y * back->pitch + x0],
               (unsigned)count * sizeof(uint32_t));
    }
}

int
comp_composite(compositor_t *c)
{
    /* Collect dirty rects from windows */
    for (int i = 0; i < c->nwindows; i++) {
        glyph_window_t *win = c->windows[i];
        if (!win->visible)
            continue;
        glyph_rect_t wr;
        if (glyph_window_get_dirty_rect(win, &wr)) {
            /* Convert window-local dirty rect to screen coords */
            wr.x += win->x;
            wr.y += win->y;
            comp_add_dirty(c, wr);
        }
    }

    /* Full redraw path (first frame, window raise, etc.) */
    if (c->full_redraw) {
        /* Desktop background — wallpaper or solid fill */
        if (c->wallpaper.pixels) {
            if ((int)c->wallpaper.w == c->back.w &&
                (int)c->wallpaper.h == c->back.h) {
                /* Exact match — memcpy rows (pitch may differ from width) */
                for (int y = 0; y < c->back.h; y++)
                    memcpy(&c->back.buf[y * c->back.pitch],
                           &c->wallpaper.pixels[y * c->back.w],
                           (size_t)c->back.w * sizeof(uint32_t));
            } else {
                draw_blit_scaled(&c->back, 0, 0, c->back.w, c->back.h,
                                 c->wallpaper.pixels,
                                 (int)c->wallpaper.w, (int)c->wallpaper.h);
            }
        } else {
            draw_fill_rect(&c->back, 0, 0, c->back.w, c->back.h, C_BG1);
        }

        /* Desktop decorations */
        if (c->on_draw_desktop)
            c->on_draw_desktop(&c->back, c->back.w, c->back.h);

        /* Render and blit all windows */
        for (int i = 0; i < c->nwindows; i++) {
            glyph_window_t *win = c->windows[i];
            if (!win->visible)
                continue;
            glyph_window_mark_all_dirty(win);
            glyph_window_render(win);
            blit_window_to_back(&c->back, win, 0);
        }

        /* Desktop selection box */
        draw_selection_box(&c->back, c);

        /* Overlay (frosted glass dock etc.) -- after windows, before flip */
        if (c->on_draw_overlay)
            c->on_draw_overlay(&c->back, c->back.w, c->back.h);

        /* Full flip */
        memcpy(c->fb.buf, c->back.buf,
               (size_t)c->fb.pitch * (size_t)c->fb.h * sizeof(uint32_t));

        /* Build drag snapshot if pending: re-composite without dragged window,
         * save backbuffer, then restore the full composite to fb. */
        if (c->drag_snapshot_pending && c->drag_win) {
            c->drag_snapshot_pending = 0;

            /* Re-render background */
            if (c->wallpaper.pixels) {
                if ((int)c->wallpaper.w == c->back.w &&
                    (int)c->wallpaper.h == c->back.h) {
                    for (int y = 0; y < c->back.h; y++)
                        memcpy(&c->back.buf[y * c->back.pitch],
                               &c->wallpaper.pixels[y * c->back.w],
                               (size_t)c->back.w * sizeof(uint32_t));
                } else {
                    draw_blit_scaled(&c->back, 0, 0, c->back.w, c->back.h,
                                     c->wallpaper.pixels,
                                     (int)c->wallpaper.w, (int)c->wallpaper.h);
                }
            } else {
                draw_fill_rect(&c->back, 0, 0, c->back.w, c->back.h, C_BG1);
            }
            if (c->on_draw_desktop)
                c->on_draw_desktop(&c->back, c->back.w, c->back.h);

            /* Render all windows EXCEPT the dragged one */
            for (int i = 0; i < c->nwindows; i++) {
                glyph_window_t *w2 = c->windows[i];
                if (!w2->visible || w2 == c->drag_win)
                    continue;
                glyph_window_mark_all_dirty(w2);
                glyph_window_render(w2);
                blit_window_to_back(&c->back, w2, BLIT_FROST);
            }
            if (c->on_draw_overlay)
                c->on_draw_overlay(&c->back, c->back.w, c->back.h);

            /* Save as snapshot */
            size_t sz = (size_t)c->back.pitch * (size_t)c->back.h * sizeof(uint32_t);
            if (!c->drag_snapshot)
                c->drag_snapshot = malloc(sz);
            if (c->drag_snapshot)
                memcpy(c->drag_snapshot, c->back.buf, sz);

            /* Restore the full composite (with dragged window) to backbuffer */
            memcpy(c->back.buf, c->fb.buf, sz);
        }

        c->full_redraw = 0;
        c->ndirty = 0;
        c->bg_rendered = 1;
        return 1;
    }

    /* Skip if nothing dirty */
    if (c->ndirty == 0)
        return 0;

    /* Process each dirty rect individually instead of unioning into one
     * giant bounding box. This avoids redrawing the entire horizontal
     * span between two small dirty regions on opposite sides. */
    int saved_ndirty = c->ndirty;
    glyph_rect_t saved_rects[MAX_DIRTY_RECTS];
    for (int i = 0; i < saved_ndirty; i++)
        saved_rects[i] = c->dirty_rects[i];

    for (int ri = 0; ri < saved_ndirty; ri++) {
        glyph_rect_t dr = saved_rects[ri];

        /* Redraw background in this dirty rect */
        if (c->wallpaper.pixels) {
            if ((int)c->wallpaper.w == c->back.w &&
                (int)c->wallpaper.h == c->back.h) {
                int dy0 = dr.y < 0 ? 0 : dr.y;
                int dy1 = dr.y + dr.h;
                if (dy1 > c->back.h) dy1 = c->back.h;
                int x0 = dr.x < 0 ? 0 : dr.x;
                int x1 = dr.x + dr.w;
                if (x1 > c->back.w) x1 = c->back.w;
                int count = x1 - x0;
                if (count > 0) {
                    for (int y = dy0; y < dy1; y++)
                        memcpy(&c->back.buf[y * c->back.pitch + x0],
                               &c->wallpaper.pixels[y * c->back.w + x0],
                               (unsigned)count * sizeof(uint32_t));
                }
            } else {
                draw_blit_scaled(&c->back, 0, 0, c->back.w, c->back.h,
                                 c->wallpaper.pixels,
                                 (int)c->wallpaper.w, (int)c->wallpaper.h);
            }
        } else {
            int dy0 = dr.y < 0 ? 0 : dr.y;
            int dy1 = dr.y + dr.h;
            if (dy1 > c->back.h) dy1 = c->back.h;
            int x0 = dr.x < 0 ? 0 : dr.x;
            int x1 = dr.x + dr.w;
            if (x1 > c->back.w) x1 = c->back.w;
            for (int y = dy0; y < dy1; y++)
                for (int x = x0; x < x1; x++)
                    c->back.buf[y * c->back.pitch + x] = C_BG1;
        }
    }

    /* Desktop decorations (once — not per-rect) */
    if (c->on_draw_desktop)
        c->on_draw_desktop(&c->back, c->back.w, c->back.h);

    /* Render windows that overlap ANY dirty rect */
    for (int i = 0; i < c->nwindows; i++) {
        glyph_window_t *win = c->windows[i];
        if (!win->visible)
            continue;
        glyph_rect_t wr = win_screen_rect(win);
        int dominated = 0;
        for (int ri = 0; ri < saved_ndirty; ri++) {
            if (glyph_rect_intersects(wr, saved_rects[ri])) {
                dominated = 1;
                break;
            }
        }
        if (!dominated)
            continue;
        glyph_window_render(win);
        blit_window_to_back(&c->back, win, BLIT_FROST);
    }

    /* Overlay (frosted glass dock etc.) — once, after windows */
    if (c->on_draw_overlay)
        c->on_draw_overlay(&c->back, c->back.w, c->back.h);

    /* Partial flip — per dirty rect */
    for (int ri = 0; ri < saved_ndirty; ri++)
        partial_flip(&c->fb, &c->back, saved_rects[ri]);

    c->ndirty = 0;
    return 1;
}

/* ---- Mouse handling ---- */

/* Hit-test the close button (red circle in titlebar) */
static int
hit_close_button(glyph_window_t *win, int mx, int my)
{
    /* Circle center: same as render_chrome btn_x + BTN_RADIUS */
    int cx = win->x + GLYPH_BORDER_WIDTH + 8 + 7;
    int cy = win->y + (GLYPH_TITLEBAR_HEIGHT + GLYPH_BORDER_WIDTH) / 2;
    int dx = mx - cx, dy = my - cy;
    return dx * dx + dy * dy <= 10 * 10;
}

/* Hit-test the titlebar area of a glyph window */
static int
hit_titlebar(glyph_window_t *win, int mx, int my)
{
    int tb_x = win->x + GLYPH_BORDER_WIDTH;
    int tb_y = win->y + GLYPH_BORDER_WIDTH;
    int tb_w = win->client_w;
    int tb_h = GLYPH_TITLEBAR_HEIGHT;
    return mx >= tb_x && mx < tb_x + tb_w &&
           my >= tb_y && my < tb_y + tb_h;
}

void
comp_handle_mouse(compositor_t *c, uint8_t buttons, int16_t dx, int16_t dy)
{
    int left = buttons & 1;
    int prev_left = c->prev_buttons & 1;
    int old_cx = c->cursor_x;
    int old_cy = c->cursor_y;

    /* Update cursor position with speed multiplier (1.5x via integer math) */
    c->cursor_x += dx + dx / 2;
    c->cursor_y += dy + dy / 2;
    if (c->cursor_x < 0) c->cursor_x = 0;
    if (c->cursor_y < 0) c->cursor_y = 0;
    if (c->cursor_x >= c->fb.w) c->cursor_x = c->fb.w - 1;
    if (c->cursor_y >= c->fb.h) c->cursor_y = c->fb.h - 1;

    /* If cursor moved, add old and new cursor rects as dirty */
    if (c->cursor_x != old_cx || c->cursor_y != old_cy) {
        glyph_rect_t old_r = { old_cx, old_cy, CURSOR_W, CURSOR_H };
        glyph_rect_t new_r = { c->cursor_x, c->cursor_y, CURSOR_W, CURSOR_H };
        comp_add_dirty(c, old_r);
        comp_add_dirty(c, new_r);
    }

    /* Desktop selection in progress */
    if (c->selecting && left) {
        glyph_rect_t old_sel = {
            c->sel_x0 < c->sel_x1 ? c->sel_x0 : c->sel_x1,
            c->sel_y0 < c->sel_y1 ? c->sel_y0 : c->sel_y1,
            abs(c->sel_x1 - c->sel_x0) + 1,
            abs(c->sel_y1 - c->sel_y0) + 1
        };
        c->sel_x1 = c->cursor_x;
        c->sel_y1 = c->cursor_y;
        glyph_rect_t new_sel = {
            c->sel_x0 < c->sel_x1 ? c->sel_x0 : c->sel_x1,
            c->sel_y0 < c->sel_y1 ? c->sel_y0 : c->sel_y1,
            abs(c->sel_x1 - c->sel_x0) + 1,
            abs(c->sel_y1 - c->sel_y0) + 1
        };
        comp_add_dirty(c, old_sel);
        comp_add_dirty(c, new_sel);
        c->prev_buttons = buttons;
        return;
    }

    /* Desktop selection released */
    if (c->selecting && !left) {
        glyph_rect_t sel_r = {
            c->sel_x0 < c->sel_x1 ? c->sel_x0 : c->sel_x1,
            c->sel_y0 < c->sel_y1 ? c->sel_y0 : c->sel_y1,
            abs(c->sel_x1 - c->sel_x0) + 1,
            abs(c->sel_y1 - c->sel_y0) + 1
        };
        comp_add_dirty(c, sel_r);
        c->selecting = 0;
        c->prev_buttons = buttons;
        return;
    }

    /* Content drag in progress (mouse selection in client area) */
    if (c->content_drag_win && left) {
        int local_x = c->cursor_x - c->content_drag_win->x;
        int local_y = c->cursor_y - c->content_drag_win->y;
        if (c->content_drag_win->on_mouse_move)
            c->content_drag_win->on_mouse_move(c->content_drag_win, local_x, local_y);
        c->prev_buttons = buttons;
        return;
    }

    /* Content drag released */
    if (c->content_drag_win && !left) {
        int local_x = c->cursor_x - c->content_drag_win->x;
        int local_y = c->cursor_y - c->content_drag_win->y;
        if (c->content_drag_win->on_mouse_up)
            c->content_drag_win->on_mouse_up(c->content_drag_win, local_x, local_y);
        c->content_drag_win = NULL;
        c->prev_buttons = buttons;
        return;
    }

    /* Titlebar drag in progress — use snapshot for zero-cost moves */
    if (c->dragging && left) {
        if (c->drag_win && c->drag_snapshot) {
            glyph_rect_t old_r = win_screen_rect(c->drag_win);

            /* Restore old position from snapshot (both backbuffer and fb) */
            for (int y = old_r.y; y < old_r.y + old_r.h && y < c->fb.h; y++) {
                if (y < 0) continue;
                int x0 = old_r.x < 0 ? 0 : old_r.x;
                int x1 = old_r.x + old_r.w;
                if (x1 > c->fb.w) x1 = c->fb.w;
                if (x1 > x0) {
                    memcpy(&c->back.buf[y * c->back.pitch + x0],
                           &c->drag_snapshot[y * c->back.pitch + x0],
                           (unsigned)(x1 - x0) * sizeof(uint32_t));
                    memcpy(&c->fb.buf[y * c->fb.pitch + x0],
                           &c->drag_snapshot[y * c->back.pitch + x0],
                           (unsigned)(x1 - x0) * sizeof(uint32_t));
                }
            }

            /* Move window */
            c->drag_win->x = c->cursor_x - c->drag_dx;
            c->drag_win->y = c->cursor_y - c->drag_dy;

            /* Render dragged window at new position (opaque) */
            glyph_window_render(c->drag_win);
            blit_window_to_back(&c->back, c->drag_win, BLIT_OPAQUE);

            /* Flip new position to framebuffer */
            glyph_rect_t new_r = win_screen_rect(c->drag_win);
            partial_flip(&c->fb, &c->back, new_r);
        }
        c->prev_buttons = buttons;
        return;
    }

    /* Titlebar drag released — free snapshot, restore full frost */
    if (c->dragging && !left) {
        glyph_window_t *dw = c->drag_win;
        c->dragging = 0;
        c->drag_win = NULL;
        if (c->drag_snapshot) {
            free(c->drag_snapshot);
            c->drag_snapshot = NULL;
        }
        if (dw) {
            glyph_window_mark_all_dirty(dw);
            c->full_redraw = 1;
        }
        c->prev_buttons = buttons;
        return;
    }

    /* Button press edge (0 -> 1) */
    if (left && !prev_left) {
        glyph_window_t *win = comp_window_at(c, c->cursor_x, c->cursor_y);
        if (win) {
            /* Close button */
            if (win->closeable && hit_close_button(win, c->cursor_x, c->cursor_y)) {
                comp_remove_window(c, win);
                c->prev_buttons = buttons;
                return;
            }

            /* Titlebar drag */
            if (hit_titlebar(win, c->cursor_x, c->cursor_y)) {
                c->dragging = 1;
                c->drag_win = win;
                c->drag_dx = c->cursor_x - win->x;
                c->drag_dy = c->cursor_y - win->y;
                if (c->focused != win) {
                    if (c->focused)
                        c->focused->focused_window = 0;
                    c->focused = win;
                    win->focused_window = 1;
                    comp_raise_window(c, win);
                    for (int i = 0; i < c->nwindows; i++)
                        glyph_window_mark_all_dirty(c->windows[i]);
                }

                /* Snapshot: on the next comp_composite, do a full redraw
                 * (which shows the raised/focused state), then build a
                 * snapshot without the dragged window for restore. */
                c->full_redraw = 1;
                c->drag_snapshot_pending = 1;
                for (int i = 0; i < c->nwindows; i++)
                    glyph_window_mark_all_dirty(c->windows[i]);

                c->prev_buttons = buttons;
                return;
            }

            /* Click on window */
            if (c->focused != win) {
                if (c->focused) {
                    c->focused->focused_window = 0;
                    glyph_window_mark_all_dirty(c->focused);
                }
                c->focused = win;
                win->focused_window = 1;
                comp_raise_window(c, win);
                glyph_window_mark_all_dirty(win);
            }

            /* Dispatch to glyph window (converts to window-local coords) */
            int local_x = c->cursor_x - win->x;
            int local_y = c->cursor_y - win->y;

            /* Content area mouse-down: start content drag for text selection.
             * Only for terminal windows (have on_mouse_down), not widget windows. */
            if (win->on_mouse_down && win->priv) {
                win->on_mouse_down(win, local_x, local_y);
                c->content_drag_win = win;
            }

            glyph_window_dispatch_mouse(win, 1, local_x, local_y);
        } else {
            /* Click on empty desktop — start selection box */
            c->selecting = 1;
            c->sel_x0 = c->cursor_x;
            c->sel_y0 = c->cursor_y;
            c->sel_x1 = c->cursor_x;
            c->sel_y1 = c->cursor_y;
        }
    }

    c->prev_buttons = buttons;
}

void
comp_handle_key(compositor_t *c, char key)
{
    if (c->focused)
        glyph_window_dispatch_key(c->focused, key);
}
