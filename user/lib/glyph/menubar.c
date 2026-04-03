/* menubar.c -- Horizontal menu bar with dropdown menus for Glyph toolkit */
#include "glyph.h"
#include <stdlib.h>
#include <string.h>

#define MB_HEIGHT    (glyph_text_height() + 4)
#define MB_PAD       8
#define MB_BG        0x001C2430
#define MB_FG        C_TEXT
#define MB_OPEN_BG   C_WIN_BG
#define MB_HOVER_BG  C_SEL_BG
#define MB_HOVER_FG  0x00FFFFFF
#define MB_BORDER    0x00303848
#define MB_DD_BG     C_WIN_BG
#define MB_DD_BORDER C_INPUT_BD

/* Compute x position of menu label i */
static int
menu_label_x(glyph_menubar_t *mb, int i)
{
    int x = MB_PAD;
    for (int j = 0; j < i; j++) {
        int len = 0;
        const char *p = mb->menus[j].label;
        while (*p++) len++;
        x += glyph_text_width(mb->menus[j].label) + 2 * MB_PAD;
    }
    return x;
}

static int
menu_label_w(glyph_menubar_t *mb, int i)
{
    return glyph_text_width(mb->menus[i].label) + 2 * MB_PAD;
}

static void
menubar_draw(glyph_widget_t *self, surface_t *surf, int ox, int oy)
{
    glyph_menubar_t *mb = (glyph_menubar_t *)self;

    /* Bar background */
    draw_fill_rect(surf, ox, oy, self->w, MB_HEIGHT, MB_BG);
    /* Bottom border */
    for (int i = 0; i < self->w; i++)
        draw_px(surf, ox + i, oy + MB_HEIGHT - 1, MB_BORDER);

    /* Menu labels */
    for (int i = 0; i < mb->nmenu; i++) {
        int lx = ox + menu_label_x(mb, i);
        int lw = menu_label_w(mb, i);

        if (i == mb->open_idx) {
            draw_fill_rect(surf, lx, oy, lw, MB_HEIGHT - 1, MB_OPEN_BG);
        }

        draw_text_ui(surf, lx + MB_PAD, oy + 2, mb->menus[i].label, MB_FG);
    }

    /* Dropdown for open menu */
    if (mb->open_idx >= 0 && mb->open_idx < mb->nmenu) {
        glyph_menu_def_t *menu = &mb->menus[mb->open_idx];
        int dd_x = ox + menu_label_x(mb, mb->open_idx);
        int dd_y = oy + MB_HEIGHT;

        /* Find widest item */
        int max_w = 0;
        for (int i = 0; i < menu->count; i++) {
            int w = glyph_text_width(menu->items[i]) + 2 * MB_PAD;
            if (w > max_w) max_w = w;
        }
        if (max_w < menu_label_w(mb, mb->open_idx))
            max_w = menu_label_w(mb, mb->open_idx);

        int dd_h = menu->count * glyph_text_height() + 4;

        /* Dropdown background */
        draw_fill_rect(surf, dd_x, dd_y, max_w, dd_h, MB_DD_BG);
        draw_rect(surf, dd_x, dd_y, max_w, dd_h, MB_DD_BORDER);

        /* Items */
        for (int i = 0; i < menu->count; i++) {
            draw_text_ui(surf, dd_x + MB_PAD, dd_y + 2 + i * glyph_text_height(),
                        menu->items[i], MB_FG);
        }
    }
}

static void
menubar_on_mouse(glyph_widget_t *self, int btn, int local_x, int local_y)
{
    (void)btn;

    glyph_menubar_t *mb = (glyph_menubar_t *)self;

    /* Click in bar area */
    if (local_y < MB_HEIGHT) {
        for (int i = 0; i < mb->nmenu; i++) {
            int lx = menu_label_x(mb, i);
            int lw = menu_label_w(mb, i);
            if (local_x >= lx && local_x < lx + lw) {
                mb->open_idx = (mb->open_idx == i) ? -1 : i;
                glyph_widget_mark_dirty(self);
                return;
            }
        }
        /* Click on bar but not on a label -- close */
        mb->open_idx = -1;
        glyph_widget_mark_dirty(self);
        return;
    }

    /* Click in dropdown area */
    if (mb->open_idx >= 0 && mb->open_idx < mb->nmenu) {
        glyph_menu_def_t *menu = &mb->menus[mb->open_idx];
        int dd_y = MB_HEIGHT;
        int item_idx = (local_y - dd_y - 2) / glyph_text_height();
        if (item_idx >= 0 && item_idx < menu->count) {
            int saved_menu = mb->open_idx;
            mb->open_idx = -1;
            glyph_widget_mark_dirty(self);
            if (mb->on_select)
                mb->on_select(self, saved_menu, item_idx);
            return;
        }
    }

    /* Click elsewhere -- close */
    mb->open_idx = -1;
    glyph_widget_mark_dirty(self);
}

static void
menubar_on_key(glyph_widget_t *self, char key)
{
    glyph_menubar_t *mb = (glyph_menubar_t *)self;

    /* Escape closes open menu */
    if (key == 27) {
        if (mb->open_idx >= 0) {
            mb->open_idx = -1;
            glyph_widget_mark_dirty(self);
        }
    }
}

glyph_menubar_t *
glyph_menubar_create(void (*on_select)(glyph_widget_t *, int, int))
{
    glyph_menubar_t *mb = calloc(1, sizeof(*mb));
    if (!mb)
        return NULL;

    glyph_widget_init(&mb->base, GLYPH_WIDGET_MENUBAR);
    mb->base.draw_fn = menubar_draw;
    mb->base.on_mouse = menubar_on_mouse;
    mb->base.on_key = menubar_on_key;
    mb->base.focusable = 1;
    mb->on_select = on_select;
    mb->open_idx = -1;

    mb->base.pref_h = MB_HEIGHT;
    mb->base.h = MB_HEIGHT;

    return mb;
}

void
glyph_menubar_add_menu(glyph_menubar_t *mb, const char *label, const char **items, int count)
{
    if (!mb || mb->nmenu >= GLYPH_MENU_MAX_MENUS)
        return;

    glyph_menu_def_t *menu = &mb->menus[mb->nmenu];

    /* Copy label */
    int len = 0;
    if (label) {
        while (label[len] && len < 31) {
            menu->label[len] = label[len];
            len++;
        }
    }
    menu->label[len] = '\0';

    /* Copy items */
    if (count > GLYPH_MENU_MAX_ITEMS)
        count = GLYPH_MENU_MAX_ITEMS;
    for (int i = 0; i < count; i++) {
        int ilen = 0;
        if (items[i]) {
            while (items[i][ilen] && ilen < 31) {
                menu->items[i][ilen] = items[i][ilen];
                ilen++;
            }
        }
        menu->items[i][ilen] = '\0';
    }
    menu->count = count;

    mb->nmenu++;

    /* Update width */
    int total_w = MB_PAD;
    for (int i = 0; i < mb->nmenu; i++)
        total_w += menu_label_w(mb, i);
    total_w += MB_PAD;
    mb->base.pref_w = total_w;
    mb->base.w = total_w;

    glyph_widget_mark_dirty(&mb->base);
}
