/* label.c -- Static text label widget for Glyph toolkit */
#include "glyph.h"
#include <stdlib.h>
#include <string.h>

static void
label_draw(glyph_widget_t *self, surface_t *surf, int ox, int oy)
{
    glyph_label_t *label = (glyph_label_t *)self;
    if (!label->transparent)
        draw_fill_rect(surf, ox, oy, self->w, self->h, label->bg_color);
    draw_text_ui(surf, ox, oy, label->text, label->color);
}

glyph_label_t *
glyph_label_create(const char *text, uint32_t color)
{
    glyph_label_t *label = calloc(1, sizeof(*label));
    if (!label)
        return NULL;

    glyph_widget_init(&label->base, GLYPH_WIDGET_LABEL);
    label->base.draw_fn = label_draw;
    label->base.focusable = 0;
    label->color = color;
    label->bg_color = C_WIN_BG;
    label->transparent = 1;  /* default transparent — frost shows through */

    if (text) {
        int len = 0;
        while (text[len] && len < 127) {
            label->text[len] = text[len];
            len++;
        }
        label->text[len] = '\0';
        label->base.pref_w = glyph_text_width(text);
        label->base.pref_h = glyph_text_height();
        label->base.w = label->base.pref_w;
        label->base.h = label->base.pref_h;
    }

    return label;
}

void
glyph_label_set_text(glyph_label_t *label, const char *text)
{
    if (!label)
        return;
    int len = 0;
    if (text) {
        while (text[len] && len < 127) {
            label->text[len] = text[len];
            len++;
        }
    }
    label->text[len] = '\0';
    label->base.pref_w = glyph_text_width(label->text);
    label->base.w = label->base.pref_w;
    glyph_widget_mark_dirty(&label->base);
}

void
glyph_label_set_bg(glyph_label_t *label, uint32_t bg)
{
    if (!label)
        return;
    label->bg_color = bg;
    glyph_widget_mark_dirty(&label->base);
}

void
glyph_label_set_transparent(glyph_label_t *label, int transparent)
{
    if (!label)
        return;
    label->transparent = transparent;
    glyph_widget_mark_dirty(&label->base);
}
