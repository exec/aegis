/* button.c -- Clickable button widget for Glyph toolkit */
#include "glyph.h"
#include <stdlib.h>
#include <string.h>

#define BTN_PAD_X 12
#define BTN_PAD_Y 4

#define BTN_BG_NORMAL   0x002A3850
#define BTN_BG_HOVER    0x00344868
#define BTN_BG_PRESSED  0x00203040
#define BTN_BG_DISABLED 0x00282830
#define BTN_BORDER      0x00405068
#define BTN_FG           0x00E8E8F0
#define BTN_FG_DISABLED  0x00606068

static void
button_draw(glyph_widget_t *self, surface_t *surf, int ox, int oy)
{
    glyph_button_t *btn = (glyph_button_t *)self;
    uint32_t bg, fg;

    switch (btn->state) {
    case GLYPH_BTN_HOVER:
        bg = BTN_BG_HOVER;
        fg = BTN_FG;
        break;
    case GLYPH_BTN_PRESSED:
        bg = BTN_BG_PRESSED;
        fg = BTN_FG;
        break;
    case GLYPH_BTN_DISABLED:
        bg = BTN_BG_DISABLED;
        fg = BTN_FG_DISABLED;
        break;
    default:
        bg = BTN_BG_NORMAL;
        fg = BTN_FG;
        break;
    }

    /* Subtle blend background */
    draw_blend_rect(surf, ox, oy, self->w, self->h, bg, 180);

    /* Border */
    draw_blend_rect(surf, ox, oy, self->w, 1, 0x00FFFFFF, 20);
    draw_blend_rect(surf, ox, oy + self->h - 1, self->w, 1, 0x00000000, 30);
    draw_blend_rect(surf, ox, oy, 1, self->h, 0x00FFFFFF, 15);
    draw_blend_rect(surf, ox + self->w - 1, oy, 1, self->h, 0x00000000, 20);

    /* Pressed: offset text by 1px for sunken effect */
    int text_off = (btn->state == GLYPH_BTN_PRESSED) ? 1 : 0;

    /* Center text */
    int tw = glyph_text_width(btn->text);
    int th = glyph_text_height();
    int tx = ox + (self->w - tw) / 2 + text_off;
    int ty = oy + (self->h - th) / 2 + text_off;
    draw_text_ui(surf, tx, ty, btn->text, fg);
}

static void
button_on_mouse(glyph_widget_t *self, int btn, int local_x, int local_y)
{
    (void)btn;
    (void)local_x;
    (void)local_y;

    glyph_button_t *button = (glyph_button_t *)self;
    if (button->state == GLYPH_BTN_DISABLED)
        return;

    /* On click (mouse press), fire callback */
    if (button->on_click)
        button->on_click(self);
}

glyph_button_t *
glyph_button_create(const char *text, void (*on_click)(glyph_widget_t *))
{
    glyph_button_t *btn = calloc(1, sizeof(*btn));
    if (!btn)
        return NULL;

    glyph_widget_init(&btn->base, GLYPH_WIDGET_BUTTON);
    btn->base.draw_fn = button_draw;
    btn->base.on_mouse = button_on_mouse;
    btn->base.focusable = 1;
    btn->on_click = on_click;
    btn->state = GLYPH_BTN_NORMAL;

    if (text) {
        int len = 0;
        while (text[len] && len < 63) {
            btn->text[len] = text[len];
            len++;
        }
        btn->text[len] = '\0';
        btn->base.pref_w = glyph_text_width(text) + 2 * BTN_PAD_X;
        btn->base.pref_h = glyph_text_height() + 2 * BTN_PAD_Y;
    } else {
        btn->base.pref_w = 2 * BTN_PAD_X;
        btn->base.pref_h = glyph_text_height() + 2 * BTN_PAD_Y;
    }
    btn->base.w = btn->base.pref_w;
    btn->base.h = btn->base.pref_h;

    return btn;
}

void
glyph_button_set_text(glyph_button_t *btn, const char *text)
{
    if (!btn)
        return;
    int len = 0;
    if (text) {
        while (text[len] && len < 63) {
            btn->text[len] = text[len];
            len++;
        }
    }
    btn->text[len] = '\0';
    btn->base.pref_w = glyph_text_width(btn->text) + 2 * BTN_PAD_X;
    btn->base.w = btn->base.pref_w;
    glyph_widget_mark_dirty(&btn->base);
}

void
glyph_button_set_state(glyph_button_t *btn, glyph_btn_state_t state)
{
    if (!btn || btn->state == state)
        return;
    btn->state = state;
    glyph_widget_mark_dirty(&btn->base);
}
