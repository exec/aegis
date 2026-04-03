/* image.c -- Raw pixel buffer display widget for Glyph toolkit */
#include "glyph.h"
#include <stdlib.h>

static void
image_draw(glyph_widget_t *self, surface_t *surf, int ox, int oy)
{
    glyph_image_t *img = (glyph_image_t *)self;
    if (img->pixels)
        draw_blit(surf, ox, oy, img->pixels, img->img_w, img->img_h);
}

glyph_image_t *
glyph_image_create(uint32_t *pixels, int w, int h)
{
    glyph_image_t *img = calloc(1, sizeof(*img));
    if (!img)
        return NULL;

    glyph_widget_init(&img->base, GLYPH_WIDGET_IMAGE);
    img->base.draw_fn = image_draw;
    img->base.focusable = 0;
    img->pixels = pixels;
    img->img_w = w;
    img->img_h = h;

    img->base.pref_w = w;
    img->base.pref_h = h;
    img->base.w = w;
    img->base.h = h;

    return img;
}
