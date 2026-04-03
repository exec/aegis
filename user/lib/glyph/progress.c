/* progress.c -- Progress bar widget for Glyph toolkit */
#include "glyph.h"
#include <stdlib.h>

#define PROG_HEIGHT  20
#define PROG_BG      C_INPUT_BG
#define PROG_FG      C_ACCENT
#define PROG_BORDER  C_INPUT_BD

static void
progress_draw(glyph_widget_t *self, surface_t *surf, int ox, int oy)
{
    glyph_progress_t *bar = (glyph_progress_t *)self;

    /* Clear to key color first (prevent blend accumulation on redraw) */
    draw_fill_rect(surf, ox, oy, self->w, self->h, C_SHADOW);
    /* Subtle blend background */
    draw_blend_rect(surf, ox, oy, self->w, self->h, 0x00000010, 60);

    /* Filled portion — accent color blend */
    int fill_w = (self->w - 2) * bar->value / 100;
    if (fill_w > 0)
        draw_blend_rect(surf, ox + 1, oy + 1, fill_w, self->h - 2, PROG_FG, 200);

    /* Top/bottom inset border */
    draw_blend_rect(surf, ox, oy, self->w, 1, 0x00000000, 40);
    draw_blend_rect(surf, ox, oy + self->h - 1, self->w, 1, 0x00FFFFFF, 15);
}

glyph_progress_t *
glyph_progress_create(int width)
{
    glyph_progress_t *bar = calloc(1, sizeof(*bar));
    if (!bar)
        return NULL;

    glyph_widget_init(&bar->base, GLYPH_WIDGET_PROGRESS);
    bar->base.draw_fn = progress_draw;
    bar->base.focusable = 0;
    bar->value = 0;

    bar->base.pref_w = width;
    bar->base.pref_h = PROG_HEIGHT;
    bar->base.w = width;
    bar->base.h = PROG_HEIGHT;

    return bar;
}

void
glyph_progress_set_value(glyph_progress_t *bar, int value)
{
    if (!bar)
        return;
    if (value < 0) value = 0;
    if (value > 100) value = 100;
    if (bar->value == value)
        return;
    bar->value = value;
    glyph_widget_mark_dirty(&bar->base);
}
