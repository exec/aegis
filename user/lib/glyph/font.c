/* font.c -- TTF vector font renderer using stb_truetype
 *
 * Loads TTF files from disk, bakes ASCII glyphs into bitmap atlases
 * at requested pixel sizes, and renders anti-aliased text by alpha-
 * blending glyph coverage over existing surface pixels.
 */
#include "font.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#define MAX_BAKED_SIZES 8
#define ATLAS_W         512
#define ATLAS_H         512
#define FIRST_CHAR      32
#define NUM_CHARS        95  /* ASCII 32-126 inclusive */

typedef struct {
    int size_px;
    int ascent;       /* scaled ascent in pixels */
    int descent;      /* scaled descent in pixels (negative) */
    int line_height;  /* ascent - descent + line_gap */
    unsigned char *atlas;  /* ATLAS_W x ATLAS_H alpha bitmap */
    stbtt_bakedchar cdata[NUM_CHARS];
} baked_size_t;

struct font {
    unsigned char *ttf_data;  /* raw TTF file contents */
    stbtt_fontinfo info;
    baked_size_t sizes[MAX_BAKED_SIZES];
    int num_sizes;
};

/* Global font instances */
font_t *g_font_ui   = NULL;
font_t *g_font_mono = NULL;

/* ---- Internal helpers ---- */

static baked_size_t *
find_baked(font_t *f, int size_px)
{
    if (!f)
        return NULL;
    for (int i = 0; i < f->num_sizes; i++) {
        if (f->sizes[i].size_px == size_px)
            return &f->sizes[i];
    }
    return NULL;
}

/* ---- Public API ---- */

font_t *
font_load(const char *path)
{
    if (!path)
        return NULL;

    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return NULL;

    /* Get file size via lseek */
    off_t size = lseek(fd, 0, SEEK_END);
    if (size <= 0 || size > 16 * 1024 * 1024) {
        /* Reject empty files or files > 16 MB */
        close(fd);
        return NULL;
    }
    lseek(fd, 0, SEEK_SET);

    unsigned char *data = malloc((size_t)size);
    if (!data) {
        close(fd);
        return NULL;
    }

    /* Read entire file */
    size_t total = 0;
    while (total < (size_t)size) {
        ssize_t n = read(fd, data + total, (size_t)size - total);
        if (n <= 0) {
            free(data);
            close(fd);
            return NULL;
        }
        total += (size_t)n;
    }
    close(fd);

    font_t *f = calloc(1, sizeof(font_t));
    if (!f) {
        free(data);
        return NULL;
    }
    f->ttf_data = data;
    f->num_sizes = 0;

    if (!stbtt_InitFont(&f->info, f->ttf_data, 0)) {
        free(data);
        free(f);
        return NULL;
    }

    return f;
}

void
font_bake(font_t *f, int size_px)
{
    if (!f || size_px <= 0)
        return;

    /* Already baked? */
    if (find_baked(f, size_px))
        return;

    /* No room for more baked sizes? */
    if (f->num_sizes >= MAX_BAKED_SIZES)
        return;

    baked_size_t *bs = &f->sizes[f->num_sizes];
    memset(bs, 0, sizeof(*bs));
    bs->size_px = size_px;

    bs->atlas = calloc(1, ATLAS_W * ATLAS_H);
    if (!bs->atlas)
        return;

    int ret = stbtt_BakeFontBitmap(f->ttf_data, 0, (float)size_px,
                                   bs->atlas, ATLAS_W, ATLAS_H,
                                   FIRST_CHAR, NUM_CHARS, bs->cdata);
    if (ret <= 0) {
        /* ret < 0 means not all chars fit; absolute value is how many
         * did fit. ret == 0 means none fit. For partial success we
         * keep what we have. For total failure, free and bail. */
        if (ret == 0) {
            free(bs->atlas);
            bs->atlas = NULL;
            return;
        }
        /* Partial bake -- some chars are missing but we proceed.
         * Missing chars will render as empty space. */
    }

    /* Compute scaled vertical metrics */
    int asc, desc, lgap;
    stbtt_GetFontVMetrics(&f->info, &asc, &desc, &lgap);
    float scale = stbtt_ScaleForPixelHeight(&f->info, (float)size_px);
    bs->ascent = (int)(asc * scale + 0.5f);
    bs->descent = (int)(desc * scale - 0.5f);
    bs->line_height = (int)((asc - desc + lgap) * scale + 0.5f);

    f->num_sizes++;
}

int
font_height(font_t *f, int size_px)
{
    baked_size_t *bs = find_baked(f, size_px);
    return bs ? bs->line_height : 0;
}

int
font_ascent(font_t *f, int size_px)
{
    baked_size_t *bs = find_baked(f, size_px);
    return bs ? bs->ascent : 0;
}

int
font_text_width(font_t *f, int size_px, const char *text)
{
    baked_size_t *bs = find_baked(f, size_px);
    if (!bs || !text)
        return 0;

    int width = 0;
    while (*text) {
        int ch = (unsigned char)*text;
        if (ch >= FIRST_CHAR && ch < FIRST_CHAR + NUM_CHARS) {
            stbtt_bakedchar *bc = &bs->cdata[ch - FIRST_CHAR];
            width += (int)(bc->xadvance + 0.5f);
        }
        text++;
    }
    return width;
}

int
font_draw_char(surface_t *s, font_t *f, int size_px,
               int x, int y, char ch, uint32_t color)
{
    if (!s || !f)
        return 0;

    baked_size_t *bs = find_baked(f, size_px);
    if (!bs)
        return 0;

    int ci = (unsigned char)ch;
    if (ci < FIRST_CHAR || ci >= FIRST_CHAR + NUM_CHARS)
        return 0;

    stbtt_bakedchar *bc = &bs->cdata[ci - FIRST_CHAR];

    /* Glyph bounding box in atlas */
    int gw = bc->x1 - bc->x0;
    int gh = bc->y1 - bc->y0;

    /* Screen position: baseline at y + ascent, offset by glyph metrics */
    int sx = x + (int)(bc->xoff + 0.5f);
    int sy = y + bs->ascent + (int)(bc->yoff + 0.5f);

    /* Extract foreground color components */
    int fg_r = (color >> 16) & 0xFF;
    int fg_g = (color >> 8) & 0xFF;
    int fg_b = color & 0xFF;

    /* Blit glyph with alpha blending */
    for (int row = 0; row < gh; row++) {
        int py = sy + row;
        if (py < 0 || py >= s->h)
            continue;
        for (int col = 0; col < gw; col++) {
            int px = sx + col;
            if (px < 0 || px >= s->w)
                continue;

            int alpha = bs->atlas[(bc->y0 + row) * ATLAS_W + (bc->x0 + col)];
            if (alpha == 0)
                continue;

            uint32_t *dst = &s->buf[py * s->pitch + px];
            if (alpha == 255) {
                *dst = color;
            } else {
                uint32_t bg = *dst;
                int bg_r = (bg >> 16) & 0xFF;
                int bg_g = (bg >> 8) & 0xFF;
                int bg_b = bg & 0xFF;
                int r = (fg_r * alpha + bg_r * (255 - alpha)) / 255;
                int g = (fg_g * alpha + bg_g * (255 - alpha)) / 255;
                int b = (fg_b * alpha + bg_b * (255 - alpha)) / 255;
                *dst = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
            }
        }
    }

    return (int)(bc->xadvance + 0.5f);
}

void
font_draw_text(surface_t *s, font_t *f, int size_px,
               int x, int y, const char *text, uint32_t color)
{
    if (!s || !f || !text)
        return;

    while (*text) {
        int advance = font_draw_char(s, f, size_px, x, y, *text, color);
        x += advance;
        text++;
    }
}

void
font_init(void)
{
    g_font_ui = NULL;
    g_font_mono = NULL;

    /* Try loading UI font (Inter) */
    g_font_ui = font_load("/usr/share/fonts/Inter-Regular.ttf");
    if (g_font_ui) {
        font_bake(g_font_ui, 11);
        font_bake(g_font_ui, 12);
        font_bake(g_font_ui, 13);
        font_bake(g_font_ui, 14);
        font_bake(g_font_ui, 16);
        font_bake(g_font_ui, 20);
    }

    /* Try loading monospace font (JetBrains Mono) */
    g_font_mono = font_load("/usr/share/fonts/JetBrainsMono-Regular.ttf");
    if (g_font_mono) {
        font_bake(g_font_mono, 14);
        font_bake(g_font_mono, 16);
        font_bake(g_font_mono, 18);
        font_bake(g_font_mono, 20);
    }
}
