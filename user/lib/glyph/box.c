/* box.c -- HBox and VBox layout containers for Glyph toolkit */
#include "glyph.h"
#include <stdlib.h>

static void
box_draw(glyph_widget_t *self, surface_t *surf, int ox, int oy)
{
    /* Box containers are invisible -- children draw themselves.
     * The draw_tree walk handles children automatically. */
    (void)self;
    (void)surf;
    (void)ox;
    (void)oy;
}

glyph_box_t *
glyph_hbox_create(int padding, int spacing)
{
    glyph_box_t *box = calloc(1, sizeof(*box));
    if (!box)
        return NULL;
    glyph_widget_init(&box->base, GLYPH_WIDGET_HBOX);
    box->padding = padding;
    box->spacing = spacing;
    box->base.draw_fn = box_draw;
    return box;
}

glyph_box_t *
glyph_vbox_create(int padding, int spacing)
{
    glyph_box_t *box = calloc(1, sizeof(*box));
    if (!box)
        return NULL;
    glyph_widget_init(&box->base, GLYPH_WIDGET_VBOX);
    box->padding = padding;
    box->spacing = spacing;
    box->base.draw_fn = box_draw;
    return box;
}

void
glyph_box_layout(glyph_box_t *box)
{
    if (!box)
        return;

    glyph_widget_t *base = &box->base;
    int is_vertical = (base->type == GLYPH_WIDGET_VBOX);
    int pos = box->padding;
    int max_cross = 0; /* max width (vbox) or max height (hbox) */

    for (int i = 0; i < base->nchildren; i++) {
        glyph_widget_t *child = base->children[i];
        if (!child->visible)
            continue;

        /* Use preferred size for child dimensions */
        child->w = child->pref_w;
        child->h = child->pref_h;

        if (is_vertical) {
            child->x = box->padding;
            child->y = pos;
            pos += child->h + box->spacing;
            if (child->w > max_cross)
                max_cross = child->w;
        } else {
            child->x = pos;
            child->y = box->padding;
            pos += child->w + box->spacing;
            if (child->h > max_cross)
                max_cross = child->h;
        }

        /* Recursively layout child if it's also a box */
        if (child->type == GLYPH_WIDGET_HBOX || child->type == GLYPH_WIDGET_VBOX)
            glyph_box_layout((glyph_box_t *)child);
    }

    /* Remove trailing spacing */
    if (base->nchildren > 0)
        pos -= box->spacing;
    pos += box->padding;

    /* Update box's own size */
    if (is_vertical) {
        base->w = max_cross + 2 * box->padding;
        base->h = pos;
    } else {
        base->w = pos;
        base->h = max_cross + 2 * box->padding;
    }

    /* Also set as preferred size */
    base->pref_w = base->w;
    base->pref_h = base->h;

    glyph_widget_mark_dirty(base);
}
