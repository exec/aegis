/* scrollbar.c -- Vertical scroll bar widget for Glyph toolkit */
#include "glyph.h"
#include <stdlib.h>

#define SB_WIDTH     16
#define SB_BG        C_INPUT_BG
#define SB_THUMB     0x00404858
#define SB_THUMB_HI  0x00506070
#define SB_BORDER    C_INPUT_BD

static int
sb_thumb_h(glyph_scrollbar_t *sb)
{
    int track = sb->base.h;
    int range = sb->max_val - sb->min_val;
    if (range <= 0) return track;
    int th = sb->page_size * track / (range + sb->page_size);
    if (th < 20) th = 20;
    if (th > track) th = track;
    return th;
}

static int
sb_thumb_y(glyph_scrollbar_t *sb)
{
    int track = sb->base.h;
    int th = sb_thumb_h(sb);
    int range = sb->max_val - sb->min_val;
    if (range <= 0) return 0;
    return (int)((long)(sb->value - sb->min_val) * (track - th) / range);
}

static void
scrollbar_draw(glyph_widget_t *self, surface_t *surf, int ox, int oy)
{
    glyph_scrollbar_t *sb = (glyph_scrollbar_t *)self;

    /* Track background — subtle blend */
    draw_blend_rect(surf, ox, oy, self->w, self->h, 0x00000010, 40);

    /* Thumb */
    int th = sb_thumb_h(sb);
    int ty = sb_thumb_y(sb);
    uint32_t tc = sb->thumb_dragging ? SB_THUMB_HI : SB_THUMB;
    draw_fill_rect(surf, ox + 1, oy + ty, self->w - 2, th, tc);
}

static void
scrollbar_on_mouse(glyph_widget_t *self, int btn, int local_x, int local_y)
{
    (void)btn;
    (void)local_x;

    glyph_scrollbar_t *sb = (glyph_scrollbar_t *)self;
    int range = sb->max_val - sb->min_val;
    if (range <= 0)
        return;

    int track = self->h;
    int th = sb_thumb_h(sb);
    int usable = track - th;
    if (usable <= 0)
        return;

    int new_val = sb->min_val + (local_y - th / 2) * range / usable;
    if (new_val < sb->min_val) new_val = sb->min_val;
    if (new_val > sb->max_val) new_val = sb->max_val;

    if (new_val != sb->value) {
        sb->value = new_val;
        glyph_widget_mark_dirty(self);
        if (sb->on_scroll)
            sb->on_scroll(self, sb->value);
    }
}

glyph_scrollbar_t *
glyph_scrollbar_create(int height, void (*on_scroll)(glyph_widget_t *, int))
{
    glyph_scrollbar_t *sb = calloc(1, sizeof(*sb));
    if (!sb)
        return NULL;

    glyph_widget_init(&sb->base, GLYPH_WIDGET_SCROLLBAR);
    sb->base.draw_fn = scrollbar_draw;
    sb->base.on_mouse = scrollbar_on_mouse;
    sb->base.focusable = 0;
    sb->on_scroll = on_scroll;
    sb->min_val = 0;
    sb->max_val = 0;
    sb->value = 0;
    sb->page_size = 1;

    sb->base.pref_w = SB_WIDTH;
    sb->base.pref_h = height;
    sb->base.w = SB_WIDTH;
    sb->base.h = height;

    return sb;
}

void
glyph_scrollbar_set_range(glyph_scrollbar_t *sb, int max_val, int page_size)
{
    if (!sb)
        return;
    sb->max_val = max_val;
    sb->page_size = page_size > 0 ? page_size : 1;
    if (sb->value > sb->max_val)
        sb->value = sb->max_val;
    glyph_widget_mark_dirty(&sb->base);
}
