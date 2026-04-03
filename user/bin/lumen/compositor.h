/* compositor.h -- Lumen window management and dirty-rect compositing */
#ifndef LUMEN_COMPOSITOR_H
#define LUMEN_COMPOSITOR_H

#include <glyph.h>
#include <theme.h>
#include <stdint.h>

#define MAX_WINDOWS     16
#define MAX_DIRTY_RECTS 32

/* Wallpaper */
typedef struct {
    uint32_t *pixels;
    uint32_t w, h;
} wallpaper_t;

typedef struct {
    surface_t fb;
    surface_t back;
    glyph_window_t *windows[MAX_WINDOWS];
    int nwindows;
    glyph_window_t *focused;
    int cursor_x, cursor_y;
    int dragging;
    glyph_window_t *drag_win;
    int drag_dx, drag_dy;
    uint32_t *drag_snapshot;       /* backbuffer snapshot at drag start */
    int drag_snapshot_pending;     /* 1 = need to build snapshot on next composite */
    glyph_window_t *content_drag_win; /* window receiving mouse drag events */
    int prev_buttons;

    /* Desktop selection box (click+drag on empty desktop) */
    int selecting;
    int sel_x0, sel_y0;  /* anchor point */
    int sel_x1, sel_y1;  /* current drag point */

    /* Dirty rect accumulator */
    glyph_rect_t dirty_rects[MAX_DIRTY_RECTS];
    int ndirty;
    int full_redraw; /* force full frame composite */

    /* Background rendered flag -- skip gradient after first frame */
    int bg_rendered;

    /* Wallpaper (optional) */
    wallpaper_t wallpaper;

    /* Desktop background callback -- called after bg fill, before windows */
    void (*on_draw_desktop)(surface_t *back, int w, int h);

    /* Overlay callback -- called after windows, before flip (for frosted glass dock) */
    void (*on_draw_overlay)(surface_t *back, int w, int h);
} compositor_t;

void comp_init(compositor_t *c, uint32_t *fb, uint32_t *backbuf, int w, int h, int pitch);
void comp_add_window(compositor_t *c, glyph_window_t *win);
void comp_remove_window(compositor_t *c, glyph_window_t *win);
void comp_raise_window(compositor_t *c, glyph_window_t *win);
glyph_window_t *comp_window_at(compositor_t *c, int x, int y);
void comp_add_dirty(compositor_t *c, glyph_rect_t r);
int comp_composite(compositor_t *c);
void comp_handle_mouse(compositor_t *c, uint8_t buttons, int16_t dx, int16_t dy);
void comp_handle_key(compositor_t *c, char key);

#endif
