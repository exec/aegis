/* compositor.c — Lumen window management and compositing */
#include "compositor.h"
#include "cursor.h"
#include "draw.h"
#include <stdlib.h>
#include <string.h>

#define CLOSE_BTN_X  8
#define CLOSE_BTN_Y  7
#define BTN_RADIUS   7
#define BTN_SPACING  22

static void
draw_window(compositor_t *c, window_t *win)
{
    surface_t *s = &c->back;
    int bx = win->x - BORDER_WIDTH;
    int by = win->y - TITLEBAR_HEIGHT - BORDER_WIDTH;
    int total_w = win->w + 2 * BORDER_WIDTH;
    int total_h = win->h + TITLEBAR_HEIGHT + 2 * BORDER_WIDTH;
    int focused = (win == c->focused);

    /* shadow */
    draw_fill_rect(s, bx + SHADOW_OFFSET, by + SHADOW_OFFSET,
                   total_w, total_h, C_SHADOW);

    /* border */
    draw_fill_rect(s, bx, by, total_w, total_h, C_BORDER);

    /* titlebar gradient */
    uint32_t t1 = focused ? C_TITLE1 : C_UTITLE1;
    uint32_t t2 = focused ? C_TITLE2 : C_UTITLE2;
    draw_gradient_v(s, win->x, by + BORDER_WIDTH,
                    win->w, TITLEBAR_HEIGHT, t1, t2);

    /* title text */
    draw_text_t(s, win->x + 50, by + BORDER_WIDTH + 5,
                win->title, C_TITLE_T);

    /* traffic-light buttons */
    int btn_y = by + BORDER_WIDTH + CLOSE_BTN_Y;
    int btn_x = win->x + CLOSE_BTN_X;

    /* close (red) */
    draw_fill_rect(s, btn_x, btn_y, BTN_RADIUS * 2, BTN_RADIUS * 2, C_RED);
    if (focused) {
        draw_text_t(s, btn_x + 3, btn_y - 2, "x", 0x00FFFFFF);
    }

    /* minimize (yellow) */
    draw_fill_rect(s, btn_x + BTN_SPACING, btn_y,
                   BTN_RADIUS * 2, BTN_RADIUS * 2, C_YELLOW);

    /* maximize (green) */
    draw_fill_rect(s, btn_x + BTN_SPACING * 2, btn_y,
                   BTN_RADIUS * 2, BTN_RADIUS * 2, C_GREEN);

    /* client area background */
    draw_fill_rect(s, win->x, win->y, win->w, win->h, C_WIN);

    /* blit client pixels if present */
    if (win->pixels) {
        draw_blit(s, win->x, win->y, win->pixels, win->w, win->h);
    }
}

static int
hit_close_button(window_t *win, int mx, int my)
{
    int btn_x = win->x + CLOSE_BTN_X;
    int btn_y = win->y - TITLEBAR_HEIGHT - BORDER_WIDTH + BORDER_WIDTH + CLOSE_BTN_Y;
    return mx >= btn_x && mx < btn_x + BTN_RADIUS * 2 &&
           my >= btn_y && my < btn_y + BTN_RADIUS * 2;
}

static int
hit_titlebar(window_t *win, int mx, int my)
{
    int tb_x = win->x - BORDER_WIDTH;
    int tb_y = win->y - TITLEBAR_HEIGHT - BORDER_WIDTH;
    int total_w = win->w + 2 * BORDER_WIDTH;
    return mx >= tb_x && mx < tb_x + total_w &&
           my >= tb_y && my < tb_y + TITLEBAR_HEIGHT + BORDER_WIDTH;
}

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
    c->needs_redraw = 1;
}

void
comp_add_window(compositor_t *c, window_t *win)
{
    if (c->nwindows >= MAX_WINDOWS)
        return;
    c->windows[c->nwindows++] = win;
    c->focused = win;
    c->needs_redraw = 1;
}

void
comp_remove_window(compositor_t *c, window_t *win)
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

    for (int i = idx; i < c->nwindows - 1; i++)
        c->windows[i] = c->windows[i + 1];
    c->nwindows--;

    if (win->pixels)
        free(win->pixels);
    free(win);

    if (c->focused == win)
        c->focused = c->nwindows > 0 ? c->windows[c->nwindows - 1] : NULL;

    c->needs_redraw = 1;
}

void
comp_raise_window(compositor_t *c, window_t *win)
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
    c->needs_redraw = 1;
}

window_t *
comp_window_at(compositor_t *c, int x, int y)
{
    for (int i = c->nwindows - 1; i >= 0; i--) {
        window_t *win = c->windows[i];
        if (!win->visible)
            continue;
        int bx = win->x - BORDER_WIDTH;
        int by = win->y - TITLEBAR_HEIGHT - BORDER_WIDTH;
        int total_w = win->w + 2 * BORDER_WIDTH;
        int total_h = win->h + TITLEBAR_HEIGHT + 2 * BORDER_WIDTH;
        if (x >= bx && x < bx + total_w && y >= by && y < by + total_h)
            return win;
    }
    return NULL;
}

void
comp_composite(compositor_t *c)
{
    surface_t *s = &c->back;

    /* desktop background gradient */
    draw_gradient_v(s, 0, 0, s->w, s->h, C_BG1, C_BG2);

    /* draw windows back-to-front */
    for (int i = 0; i < c->nwindows; i++) {
        window_t *win = c->windows[i];
        if (!win->visible)
            continue;
        if (win->on_draw)
            win->on_draw(win);
        draw_window(c, win);
    }

    /* taskbar on top */
    taskbar_draw(s, s->w, s->h);

    /* flip back buffer to framebuffer */
    memcpy(c->fb.buf, c->back.buf,
           (size_t)c->fb.pitch * (size_t)c->fb.h * sizeof(uint32_t));

    /* cursor drawn directly to fb (over the flipped image) */
    cursor_show(c->cursor_x, c->cursor_y);

    c->needs_redraw = 0;
}

void
comp_handle_mouse(compositor_t *c, uint8_t buttons, int16_t dx, int16_t dy)
{
    int left = buttons & 1;
    int prev_left = c->prev_buttons & 1;

    /* update cursor position, clamp to screen */
    c->cursor_x += dx;
    c->cursor_y += dy;
    if (c->cursor_x < 0) c->cursor_x = 0;
    if (c->cursor_y < 0) c->cursor_y = 0;
    if (c->cursor_x >= c->fb.w) c->cursor_x = c->fb.w - 1;
    if (c->cursor_y >= c->fb.h) c->cursor_y = c->fb.h - 1;

    if (dx != 0 || dy != 0)
        c->needs_redraw = 1;

    /* drag in progress */
    if (c->dragging && left) {
        if (c->drag_win) {
            c->drag_win->x = c->cursor_x - c->drag_dx;
            c->drag_win->y = c->cursor_y - c->drag_dy;
            c->needs_redraw = 1;
        }
        c->prev_buttons = buttons;
        return;
    }

    /* drag released */
    if (c->dragging && !left) {
        c->dragging = 0;
        c->drag_win = NULL;
        c->prev_buttons = buttons;
        return;
    }

    /* button press edge (0 -> 1) */
    if (left && !prev_left) {
        window_t *win = comp_window_at(c, c->cursor_x, c->cursor_y);
        if (win) {
            /* close button */
            if (win->closeable && hit_close_button(win, c->cursor_x, c->cursor_y)) {
                comp_remove_window(c, win);
                c->prev_buttons = buttons;
                return;
            }

            /* titlebar drag */
            if (hit_titlebar(win, c->cursor_x, c->cursor_y)) {
                c->dragging = 1;
                c->drag_win = win;
                c->drag_dx = c->cursor_x - win->x;
                c->drag_dy = c->cursor_y - win->y;
                if (c->focused != win) {
                    c->focused = win;
                    comp_raise_window(c, win);
                }
                c->needs_redraw = 1;
                c->prev_buttons = buttons;
                return;
            }

            /* click on client area — focus, raise, forward */
            if (c->focused != win) {
                c->focused = win;
                comp_raise_window(c, win);
                c->needs_redraw = 1;
            }
            if (win->on_mouse) {
                int local_x = c->cursor_x - win->x;
                int local_y = c->cursor_y - win->y;
                win->on_mouse(win, 1, local_x, local_y);
            }
        }
    }

    c->prev_buttons = buttons;
}

void
comp_handle_key(compositor_t *c, char key)
{
    if (c->focused && c->focused->on_key)
        c->focused->on_key(c->focused, key);
}
