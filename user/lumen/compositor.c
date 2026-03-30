/* compositor.c -- Lumen window management and dirty-rect compositing */
#include "compositor.h"
#include "dock.h"
#include "cursor.h"
#include <glyph.h>
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

/* ---- Composite ---- */

static void
blit_window_to_back(surface_t *back, glyph_window_t *win)
{
    if (win->frosted) {
        /* Frosted glass: blur background behind window, apply dark tint,
         * then keyed-blit the window surface (C_TERM_BG pixels become
         * transparent, letting the frosted background show through). */
        draw_box_blur(back, win->x, win->y, win->surf_w, win->surf_h, 8);
        draw_blend_rect(back, win->x, win->y, win->surf_w, win->surf_h,
                        C_TERM_BG, 128);
        draw_blit_keyed(back, win->x, win->y, win->surface.buf,
                        win->surf_w, win->surf_h, C_TERM_BG);
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
            blit_window_to_back(&c->back, win);
        }

        /* Overlay (frosted glass dock etc.) -- after windows, before flip */
        if (c->on_draw_overlay)
            c->on_draw_overlay(&c->back, c->back.w, c->back.h);

        /* Full flip */
        memcpy(c->fb.buf, c->back.buf,
               (size_t)c->fb.pitch * (size_t)c->fb.h * sizeof(uint32_t));

        c->full_redraw = 0;
        c->ndirty = 0;
        c->bg_rendered = 1;
        return 1;
    }

    /* Skip if nothing dirty */
    if (c->ndirty == 0)
        return 0;

    /* Union all dirty rects into one bounding rect for simplicity in v0.1 */
    glyph_rect_t combined = c->dirty_rects[0];
    for (int i = 1; i < c->ndirty; i++)
        combined = glyph_rect_union(combined, c->dirty_rects[i]);

    /* Redraw background in dirty region */
    if (c->wallpaper.pixels) {
        if ((int)c->wallpaper.w == c->back.w &&
            (int)c->wallpaper.h == c->back.h) {
            /* Exact match — copy only the dirty region rows */
            int dy0 = combined.y < 0 ? 0 : combined.y;
            int dy1 = combined.y + combined.h;
            if (dy1 > c->back.h) dy1 = c->back.h;
            int x0 = combined.x < 0 ? 0 : combined.x;
            int x1 = combined.x + combined.w;
            if (x1 > c->back.w) x1 = c->back.w;
            int count = x1 - x0;
            if (count > 0) {
                for (int y = dy0; y < dy1; y++)
                    memcpy(&c->back.buf[y * c->back.pitch + x0],
                           &c->wallpaper.pixels[y * c->back.w + x0],
                           (unsigned)count * sizeof(uint32_t));
            }
        } else {
            /* Scaled wallpaper — expensive, but correct */
            draw_blit_scaled(&c->back, 0, 0, c->back.w, c->back.h,
                             c->wallpaper.pixels,
                             (int)c->wallpaper.w, (int)c->wallpaper.h);
        }
    } else {
        int dy0 = combined.y < 0 ? 0 : combined.y;
        int dy1 = combined.y + combined.h;
        if (dy1 > c->back.h) dy1 = c->back.h;
        int x0 = combined.x < 0 ? 0 : combined.x;
        int x1 = combined.x + combined.w;
        if (x1 > c->back.w) x1 = c->back.w;
        for (int y = dy0; y < dy1; y++)
            for (int x = x0; x < x1; x++)
                c->back.buf[y * c->back.pitch + x] = C_BG1;
    }

    /* Desktop decorations (redraw into dirty region) */
    if (c->on_draw_desktop)
        c->on_draw_desktop(&c->back, c->back.w, c->back.h);

    /* Render dirty windows and blit to backbuffer */
    for (int i = 0; i < c->nwindows; i++) {
        glyph_window_t *win = c->windows[i];
        if (!win->visible)
            continue;

        /* Check if window overlaps dirty region */
        glyph_rect_t wr = win_screen_rect(win);
        if (!glyph_rect_intersects(wr, combined))
            continue;

        glyph_window_render(win);
        blit_window_to_back(&c->back, win);
    }

    /* Overlay (frosted glass dock etc.) -- after windows, before flip */
    if (c->on_draw_overlay)
        c->on_draw_overlay(&c->back, c->back.w, c->back.h);

    /* Partial flip */
    partial_flip(&c->fb, &c->back, combined);

    c->ndirty = 0;
    return 1;
}

/* ---- Mouse handling ---- */

/* Hit-test the close button (red circle in titlebar) */
static int
hit_close_button(glyph_window_t *win, int mx, int my)
{
    int btn_x = win->x + GLYPH_BORDER_WIDTH + 8;
    int btn_y = win->y + GLYPH_BORDER_WIDTH + 7;
    return mx >= btn_x && mx < btn_x + 14 &&
           my >= btn_y && my < btn_y + 14;
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

    /* Drag in progress */
    if (c->dragging && left) {
        if (c->drag_win) {
            glyph_rect_t old_r = win_screen_rect(c->drag_win);
            c->drag_win->x = c->cursor_x - c->drag_dx;
            c->drag_win->y = c->cursor_y - c->drag_dy;
            glyph_rect_t new_r = win_screen_rect(c->drag_win);
            comp_add_dirty(c, old_r);
            comp_add_dirty(c, new_r);
            glyph_window_mark_all_dirty(c->drag_win);
        }
        c->prev_buttons = buttons;
        return;
    }

    /* Drag released */
    if (c->dragging && !left) {
        c->dragging = 0;
        c->drag_win = NULL;
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
                    /* Mark both windows dirty for title bar color change */
                    for (int i = 0; i < c->nwindows; i++)
                        glyph_window_mark_all_dirty(c->windows[i]);
                }
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
            glyph_window_dispatch_mouse(win, 1, local_x, local_y);
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
