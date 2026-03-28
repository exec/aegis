/* compositor.h — Lumen window management and compositing */
#ifndef LUMEN_COMPOSITOR_H
#define LUMEN_COMPOSITOR_H

#include "draw.h"
#include <stdint.h>

#define MAX_WINDOWS     16
#define TITLEBAR_HEIGHT 30
#define BORDER_WIDTH    1
#define SHADOW_OFFSET   4
#define TASKBAR_HEIGHT  36

typedef struct window {
    int x, y;
    int w, h;
    uint32_t *pixels;
    char title[64];
    int visible;
    int closeable;
    void (*on_key)(struct window *self, char key);
    void (*on_mouse)(struct window *self, int btn, int x, int y);
    void (*on_draw)(struct window *self);
    void *priv;
} window_t;

typedef struct {
    surface_t fb;
    surface_t back;
    window_t *windows[MAX_WINDOWS];
    int nwindows;
    window_t *focused;
    int cursor_x, cursor_y;
    int dragging;
    window_t *drag_win;
    int drag_dx, drag_dy;
    int needs_redraw;
    int prev_buttons;
} compositor_t;

void comp_init(compositor_t *c, uint32_t *fb, uint32_t *backbuf, int w, int h, int pitch);
void comp_add_window(compositor_t *c, window_t *win);
void comp_remove_window(compositor_t *c, window_t *win);
void comp_raise_window(compositor_t *c, window_t *win);
window_t *comp_window_at(compositor_t *c, int x, int y);
void comp_composite(compositor_t *c);
void comp_handle_mouse(compositor_t *c, uint8_t buttons, int16_t dx, int16_t dy);
void comp_handle_key(compositor_t *c, char key);
void taskbar_draw(surface_t *s, int screen_w, int screen_h);

#endif
