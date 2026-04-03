/* listview.c -- Scrollable list view widget for Glyph toolkit */
#include "glyph.h"
#include <stdlib.h>
#include <string.h>

#define LV_ROW_H    glyph_text_height()
#define LV_PAD      2
#define LV_BG       C_INPUT_BG
#define LV_FG       C_TEXT
#define LV_SEL_BG   C_SEL_BG
#define LV_SEL_FG   0x00FFFFFF
#define LV_BORDER   C_INPUT_BD
#define LV_SB_W     16

static void
listview_draw(glyph_widget_t *self, surface_t *surf, int ox, int oy)
{
    glyph_listview_t *lv = (glyph_listview_t *)self;

    /* List area (excluding scrollbar) */
    int list_w = self->w - LV_SB_W;

    /* Clear to key color (prevent blend accumulation on scroll redraw) */
    draw_fill_rect(surf, ox, oy, list_w, self->h, C_SHADOW);
    /* Subtle blend background */
    draw_blend_rect(surf, ox, oy, list_w, self->h, 0x00000010, 60);
    draw_blend_rect(surf, ox, oy, list_w, 1, 0x00000000, 40);
    draw_blend_rect(surf, ox, oy + self->h - 1, list_w, 1, 0x00FFFFFF, 15);

    /* Rows */
    for (int i = 0; i < lv->visible_rows; i++) {
        int item_idx = lv->scroll_offset + i;
        if (item_idx >= lv->count)
            break;

        int ry = oy + LV_PAD + i * LV_ROW_H;
        int selected = (item_idx == lv->selected);

        if (selected)
            draw_blend_rect(surf, ox + 1, ry, list_w - 2, LV_ROW_H,
                            0x00C0C0E0, 25);

        uint32_t fg = selected ? LV_SEL_FG : LV_FG;
        if (lv->items[item_idx])
            draw_text_ui(surf, ox + LV_PAD + 2, ry, lv->items[item_idx], fg);
    }
}

static void
lv_scroll_cb(glyph_widget_t *self, int value)
{
    /* The scrollbar's parent is the listview (or sibling in an hbox).
     * We use the scrollbar's on_scroll to update the listview. */
    /* Walk up to find listview -- the scrollbar is a child of the listview */
    glyph_widget_t *parent = self->parent;
    if (!parent || parent->type != GLYPH_WIDGET_LISTVIEW)
        return;
    glyph_listview_t *lv = (glyph_listview_t *)parent;
    lv->scroll_offset = value;
    glyph_widget_mark_dirty(parent);
}

static void
listview_on_mouse(glyph_widget_t *self, int btn, int local_x, int local_y)
{
    (void)btn;

    glyph_listview_t *lv = (glyph_listview_t *)self;

    /* Ignore clicks in scrollbar area */
    if (local_x >= self->w - LV_SB_W)
        return;

    int row = (local_y - LV_PAD) / LV_ROW_H;
    int item_idx = lv->scroll_offset + row;
    if (item_idx >= 0 && item_idx < lv->count) {
        lv->selected = item_idx;
        glyph_widget_mark_dirty(self);
        if (lv->on_select)
            lv->on_select(self, item_idx);
    }
}

static void
listview_on_key(glyph_widget_t *self, char key)
{
    glyph_listview_t *lv = (glyph_listview_t *)self;

    /* Simple: 'j' = down, 'k' = up (vi-style since we lack arrow key support) */
    if (key == 'j' || key == 'n') {
        if (lv->selected < lv->count - 1) {
            lv->selected++;
            /* Scroll if selection goes below visible area */
            if (lv->selected >= lv->scroll_offset + lv->visible_rows)
                lv->scroll_offset = lv->selected - lv->visible_rows + 1;
            glyph_widget_mark_dirty(self);
            if (lv->on_select)
                lv->on_select(self, lv->selected);
        }
    } else if (key == 'k' || key == 'p') {
        if (lv->selected > 0) {
            lv->selected--;
            if (lv->selected < lv->scroll_offset)
                lv->scroll_offset = lv->selected;
            glyph_widget_mark_dirty(self);
            if (lv->on_select)
                lv->on_select(self, lv->selected);
        }
    }

    /* Update scrollbar if present */
    if (lv->scrollbar) {
        lv->scrollbar->value = lv->scroll_offset;
        glyph_widget_mark_dirty(&lv->scrollbar->base);
    }
}

glyph_listview_t *
glyph_listview_create(int width, int visible_rows, void (*on_select)(glyph_widget_t *, int))
{
    glyph_listview_t *lv = calloc(1, sizeof(*lv));
    if (!lv)
        return NULL;

    glyph_widget_init(&lv->base, GLYPH_WIDGET_LISTVIEW);
    lv->base.draw_fn = listview_draw;
    lv->base.on_mouse = listview_on_mouse;
    lv->base.on_key = listview_on_key;
    lv->base.focusable = 1;
    lv->on_select = on_select;
    lv->visible_rows = visible_rows;
    lv->selected = -1;

    lv->base.pref_w = width + LV_SB_W;
    lv->base.pref_h = visible_rows * LV_ROW_H + 2 * LV_PAD;
    lv->base.w = lv->base.pref_w;
    lv->base.h = lv->base.pref_h;

    /* Create internal scrollbar as a child widget */
    lv->scrollbar = glyph_scrollbar_create(lv->base.h, lv_scroll_cb);
    if (lv->scrollbar) {
        lv->scrollbar->base.x = width;
        lv->scrollbar->base.y = 0;
        glyph_widget_add_child(&lv->base, &lv->scrollbar->base);
    }

    return lv;
}

void
glyph_listview_set_items(glyph_listview_t *lv, const char **items, int count)
{
    if (!lv)
        return;
    if (count > GLYPH_LISTVIEW_MAX_ITEMS)
        count = GLYPH_LISTVIEW_MAX_ITEMS;

    for (int i = 0; i < count; i++)
        lv->items[i] = items[i];
    lv->count = count;
    lv->selected = count > 0 ? 0 : -1;
    lv->scroll_offset = 0;

    /* Update scrollbar range */
    if (lv->scrollbar) {
        int max_scroll = count - lv->visible_rows;
        if (max_scroll < 0) max_scroll = 0;
        glyph_scrollbar_set_range(lv->scrollbar, max_scroll, lv->visible_rows);
    }

    glyph_widget_mark_dirty(&lv->base);
}
