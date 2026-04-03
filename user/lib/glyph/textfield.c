/* textfield.c -- Single-line text input widget for Glyph toolkit */
#include "glyph.h"
#include <stdlib.h>
#include <string.h>

#define TF_PAD_X 4
#define TF_PAD_Y 2
#define TF_BG       C_INPUT_BG
#define TF_FG       C_TEXT
#define TF_BORDER   C_INPUT_BD
#define TF_CURSOR   C_ACCENT

static void
textfield_draw(glyph_widget_t *self, surface_t *surf, int ox, int oy)
{
    glyph_textfield_t *tf = (glyph_textfield_t *)self;

    /* Subtle blend background */
    draw_blend_rect(surf, ox, oy, self->w, self->h, 0x00000010, 80);

    /* Border */
    draw_blend_rect(surf, ox, oy, self->w, 1, 0x00000000, 40);
    draw_blend_rect(surf, ox, oy + self->h - 1, self->w, 1, 0x00FFFFFF, 15);
    draw_blend_rect(surf, ox, oy, 1, self->h, 0x00000000, 30);
    draw_blend_rect(surf, ox + self->w - 1, oy, 1, self->h, 0x00FFFFFF, 10);

    /* Text */
    int cw = glyph_char_width();
    int ch = glyph_text_height();
    int tx = ox + TF_PAD_X;
    int ty = oy + TF_PAD_Y;
    for (int i = 0; i < tf->len; i++) {
        if (tx + cw > ox + self->w - TF_PAD_X)
            break;
        char tmp[2] = { tf->buf[i], '\0' };
        draw_text_ui(surf, tx, ty, tmp, TF_FG);
        tx += cw;
    }

    /* Cursor line */
    int cx_pos = ox + TF_PAD_X + tf->cursor_pos * cw;
    if (cx_pos < ox + self->w - TF_PAD_X) {
        for (int r = 0; r < ch; r++)
            draw_px(surf, cx_pos, oy + TF_PAD_Y + r, TF_CURSOR);
    }
}

static void
textfield_on_key(glyph_widget_t *self, char key)
{
    glyph_textfield_t *tf = (glyph_textfield_t *)self;

    if (key == '\b' || key == 127) {
        /* Backspace */
        if (tf->cursor_pos > 0) {
            memmove(&tf->buf[tf->cursor_pos - 1],
                    &tf->buf[tf->cursor_pos],
                    (unsigned)(tf->len - tf->cursor_pos));
            tf->cursor_pos--;
            tf->len--;
            tf->buf[tf->len] = '\0';
            glyph_widget_mark_dirty(self);
            if (tf->on_change)
                tf->on_change(self, tf->buf);
        }
    } else if (key == '\r' || key == '\n') {
        /* Enter -- fire on_change, no insertion */
        if (tf->on_change)
            tf->on_change(self, tf->buf);
    } else if (key >= 32 && key <= 126) {
        /* Printable character */
        if (tf->len < 255) {
            memmove(&tf->buf[tf->cursor_pos + 1],
                    &tf->buf[tf->cursor_pos],
                    (unsigned)(tf->len - tf->cursor_pos));
            tf->buf[tf->cursor_pos] = key;
            tf->cursor_pos++;
            tf->len++;
            tf->buf[tf->len] = '\0';
            glyph_widget_mark_dirty(self);
            if (tf->on_change)
                tf->on_change(self, tf->buf);
        }
    }
    /* Arrow keys would need escape sequence parsing -- not available
     * with single-char key input. Deferred until ANSI key support. */
}

static void
textfield_on_mouse(glyph_widget_t *self, int btn, int local_x, int local_y)
{
    (void)btn;
    (void)local_y;

    glyph_textfield_t *tf = (glyph_textfield_t *)self;

    /* Click positions cursor */
    int char_pos = (local_x - TF_PAD_X) / glyph_char_width();
    if (char_pos < 0) char_pos = 0;
    if (char_pos > tf->len) char_pos = tf->len;
    if (char_pos != tf->cursor_pos) {
        tf->cursor_pos = char_pos;
        glyph_widget_mark_dirty(self);
    }
}

glyph_textfield_t *
glyph_textfield_create(int width_chars, void (*on_change)(glyph_widget_t *, const char *))
{
    glyph_textfield_t *tf = calloc(1, sizeof(*tf));
    if (!tf)
        return NULL;

    glyph_widget_init(&tf->base, GLYPH_WIDGET_TEXTFIELD);
    tf->base.draw_fn = textfield_draw;
    tf->base.on_key = textfield_on_key;
    tf->base.on_mouse = textfield_on_mouse;
    tf->base.focusable = 1;
    tf->on_change = on_change;
    tf->width_chars = width_chars;

    tf->base.pref_w = width_chars * glyph_char_width() + 2 * TF_PAD_X;
    tf->base.pref_h = glyph_text_height() + 2 * TF_PAD_Y;
    tf->base.w = tf->base.pref_w;
    tf->base.h = tf->base.pref_h;

    return tf;
}

const char *
glyph_textfield_get_text(glyph_textfield_t *tf)
{
    if (!tf) return "";
    return tf->buf;
}

void
glyph_textfield_set_text(glyph_textfield_t *tf, const char *text)
{
    if (!tf)
        return;
    int len = 0;
    if (text) {
        while (text[len] && len < 255) {
            tf->buf[len] = text[len];
            len++;
        }
    }
    tf->buf[len] = '\0';
    tf->len = len;
    tf->cursor_pos = len;
    glyph_widget_mark_dirty(&tf->base);
}
