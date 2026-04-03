/* tabs.c -- Tabbed container widget for Glyph toolkit */
#include "glyph.h"
#include <stdlib.h>
#include <string.h>

#define TAB_PAD      12

static int tab_header_h(void) { return glyph_text_height() + 8; }

#define TAB_BG       0x001C2430
#define TAB_ACTIVE   C_WIN_BG
#define TAB_FG       C_TEXT
#define TAB_BORDER   0x00303848

static int
tab_label_x(glyph_tabs_t *tabs, int i)
{
    int x = 0;
    for (int j = 0; j < i; j++)
        x += glyph_text_width(tabs->labels[j]) + 2 * TAB_PAD;
    return x;
}

static int
tab_label_w(glyph_tabs_t *tabs, int i)
{
    return glyph_text_width(tabs->labels[i]) + 2 * TAB_PAD;
}

static void
tabs_draw(glyph_widget_t *self, surface_t *surf, int ox, int oy)
{
    glyph_tabs_t *tabs = (glyph_tabs_t *)self;

    /* Tab header — subtle blend, not solid fill */
    draw_blend_rect(surf, ox, oy, self->w, tab_header_h(), 0x00C0C0E0, 10);

    /* Draw tab labels */
    for (int i = 0; i < tabs->ntabs; i++) {
        int lx = ox + tab_label_x(tabs, i);
        int lw = tab_label_w(tabs, i);

        if (i == tabs->active)
            draw_blend_rect(surf, lx, oy, lw, tab_header_h(), 0x00C0C0E0, 25);

        uint32_t fg = (i == tabs->active) ? 0x00FFFFFF : TAB_FG;
        draw_text_ui(surf, lx + TAB_PAD, oy + 4, tabs->labels[i], fg);
    }

    /* Subtle separator */
    draw_blend_rect(surf, ox, oy + tab_header_h() - 1, self->w, 1, 0x00FFFFFF, 15);

    /* Only the active panel's children are drawn by the tree walker.
     * We need to make inactive panels invisible. */
}

static void
tabs_on_mouse(glyph_widget_t *self, int btn, int local_x, int local_y)
{
    (void)btn;

    glyph_tabs_t *tabs = (glyph_tabs_t *)self;

    /* Click in tab header area */
    if (local_y < tab_header_h()) {
        for (int i = 0; i < tabs->ntabs; i++) {
            int lx = tab_label_x(tabs, i);
            int lw = tab_label_w(tabs, i);
            if (local_x >= lx && local_x < lx + lw) {
                if (tabs->active != i) {
                    /* Hide old panel */
                    if (tabs->active >= 0 && tabs->active < tabs->ntabs && tabs->panels[tabs->active])
                        tabs->panels[tabs->active]->visible = 0;
                    /* Show new panel */
                    tabs->active = i;
                    if (tabs->panels[i])
                        tabs->panels[i]->visible = 1;
                    glyph_widget_mark_dirty(self);
                    if (tabs->on_change)
                        tabs->on_change(self, i);
                }
                return;
            }
        }
    }
}

glyph_tabs_t *
glyph_tabs_create(void (*on_change)(glyph_widget_t *, int))
{
    glyph_tabs_t *tabs = calloc(1, sizeof(*tabs));
    if (!tabs)
        return NULL;

    glyph_widget_init(&tabs->base, GLYPH_WIDGET_TABS);
    tabs->base.draw_fn = tabs_draw;
    tabs->base.on_mouse = tabs_on_mouse;
    tabs->base.focusable = 1;
    tabs->on_change = on_change;
    tabs->active = 0;

    return tabs;
}

void
glyph_tabs_add(glyph_tabs_t *tabs, const char *label, glyph_widget_t *panel)
{
    if (!tabs || tabs->ntabs >= GLYPH_TABS_MAX)
        return;

    /* Copy label */
    int len = 0;
    if (label) {
        while (label[len] && len < 31) {
            tabs->labels[tabs->ntabs][len] = label[len];
            len++;
        }
    }
    tabs->labels[tabs->ntabs][len] = '\0';

    /* Store panel and add as child */
    tabs->panels[tabs->ntabs] = panel;
    if (panel) {
        panel->x = 0;
        panel->y = tab_header_h();
        /* Make inactive panels invisible */
        panel->visible = (tabs->ntabs == tabs->active) ? 1 : 0;
        glyph_widget_add_child(&tabs->base, panel);
    }

    tabs->ntabs++;

    /* Recompute base widget dimensions from accumulated tab headers + panels.
     * Width = max of total tab header width and widest panel.
     * Height = tab_header_h() header + tallest panel. */
    int total_tab_w = 0;
    int max_panel_w = 0;
    int max_panel_h = 0;
    for (int i = 0; i < tabs->ntabs; i++) {
        total_tab_w += tab_label_w(tabs, i);
        if (tabs->panels[i]) {
            if (tabs->panels[i]->w > max_panel_w)
                max_panel_w = tabs->panels[i]->w;
            if (tabs->panels[i]->h > max_panel_h)
                max_panel_h = tabs->panels[i]->h;
        }
    }
    int w = total_tab_w > max_panel_w ? total_tab_w : max_panel_w;
    tabs->base.w = w;
    tabs->base.h = tab_header_h() + max_panel_h;
    tabs->base.pref_w = tabs->base.w;
    tabs->base.pref_h = tabs->base.h;

    glyph_widget_mark_dirty(&tabs->base);
}
