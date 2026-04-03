/* checkbox.c -- Toggle checkbox widget for Glyph toolkit */
#include "glyph.h"
#include <stdlib.h>
#include <string.h>

#define CB_SIZE     16
#define CB_SPACING  6
#define CB_BG       C_INPUT_BG
#define CB_BORDER   C_INPUT_BD
#define CB_CHECK    C_ACCENT
#define CB_FG       C_TEXT

static void
checkbox_draw(glyph_widget_t *self, surface_t *surf, int ox, int oy)
{
    glyph_checkbox_t *cb = (glyph_checkbox_t *)self;

    /* Box */
    int bx = ox;
    int by = oy + (self->h - CB_SIZE) / 2;
    draw_blend_rect(surf, bx, by, CB_SIZE, CB_SIZE, 0x00000010, 80);
    draw_blend_rect(surf, bx, by, CB_SIZE, 1, 0x00000000, 40);
    draw_blend_rect(surf, bx, by + CB_SIZE - 1, CB_SIZE, 1, 0x00FFFFFF, 15);

    /* Checkmark */
    if (cb->checked) {
        /* Simple X-shaped checkmark */
        for (int i = 3; i < CB_SIZE - 3; i++) {
            draw_px(surf, bx + i, by + i, CB_CHECK);
            draw_px(surf, bx + i, by + CB_SIZE - 1 - i, CB_CHECK);
            draw_px(surf, bx + i + 1, by + i, CB_CHECK);
            draw_px(surf, bx + i + 1, by + CB_SIZE - 1 - i, CB_CHECK);
        }
    }

    /* Label text */
    int tx = ox + CB_SIZE + CB_SPACING;
    int ty = oy + (self->h - glyph_text_height()) / 2;
    draw_text_ui(surf, tx, ty, cb->label, CB_FG);
}

static void
checkbox_on_mouse(glyph_widget_t *self, int btn, int local_x, int local_y)
{
    (void)btn;
    (void)local_x;
    (void)local_y;

    glyph_checkbox_t *cb = (glyph_checkbox_t *)self;
    cb->checked = !cb->checked;
    glyph_widget_mark_dirty(self);
    if (cb->on_change)
        cb->on_change(self, cb->checked);
}

glyph_checkbox_t *
glyph_checkbox_create(const char *label, void (*on_change)(glyph_widget_t *, int))
{
    glyph_checkbox_t *cb = calloc(1, sizeof(*cb));
    if (!cb)
        return NULL;

    glyph_widget_init(&cb->base, GLYPH_WIDGET_CHECKBOX);
    cb->base.draw_fn = checkbox_draw;
    cb->base.on_mouse = checkbox_on_mouse;
    cb->base.focusable = 1;
    cb->on_change = on_change;
    cb->checked = 0;

    if (label) {
        int len = 0;
        while (label[len] && len < 63) {
            cb->label[len] = label[len];
            len++;
        }
        cb->label[len] = '\0';
        cb->base.pref_w = CB_SIZE + CB_SPACING + glyph_text_width(label);
    } else {
        cb->base.pref_w = CB_SIZE;
    }
    int th = glyph_text_height();
    cb->base.pref_h = th > CB_SIZE ? th : CB_SIZE;
    cb->base.w = cb->base.pref_w;
    cb->base.h = cb->base.pref_h;

    return cb;
}
