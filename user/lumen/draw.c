/* draw.c — Framebuffer drawing primitives for Lumen compositor
 *
 * All functions take a surface_t pointer and perform bounds-checked
 * pixel writes. Font rendering uses the Terminus 10x20 bitmap font.
 */
#include "draw.h"
#include "terminus20.h"

void draw_px(surface_t *s, int x, int y, uint32_t c)
{
    if (x >= 0 && x < s->w && y >= 0 && y < s->h)
        s->buf[y * s->pitch + x] = c;
}

void draw_fill_rect(surface_t *s, int x, int y, int w, int h, uint32_t c)
{
    int i, j;
    for (j = y; j < y + h && j < s->h; j++)
        for (i = x; i < x + w && i < s->w; i++)
            if (i >= 0 && j >= 0)
                s->buf[j * s->pitch + i] = c;
}

void draw_rect(surface_t *s, int x, int y, int w, int h, uint32_t c)
{
    int i;
    for (i = x; i < x + w; i++) {
        draw_px(s, i, y, c);
        draw_px(s, i, y + h - 1, c);
    }
    for (i = y; i < y + h; i++) {
        draw_px(s, x, i, c);
        draw_px(s, x + w - 1, i, c);
    }
}

void draw_gradient_v(surface_t *s, int x, int y, int w, int h,
                     uint32_t c1, uint32_t c2)
{
    int i, j;
    for (j = 0; j < h; j++) {
        int t = j * 255 / (h > 1 ? h - 1 : 1);
        uint32_t r = ((c1 >> 16 & 0xFF) * (255 - t) + (c2 >> 16 & 0xFF) * t) / 255;
        uint32_t g = ((c1 >> 8 & 0xFF) * (255 - t) + (c2 >> 8 & 0xFF) * t) / 255;
        uint32_t b = ((c1 & 0xFF) * (255 - t) + (c2 & 0xFF) * t) / 255;
        uint32_t c = (r << 16) | (g << 8) | b;
        for (i = 0; i < w; i++)
            draw_px(s, x + i, y + j, c);
    }
}

void draw_char(surface_t *s, int x, int y, char ch, uint32_t fg, uint32_t bg)
{
    const uint8_t *glyph = &font_terminus[(unsigned char)ch * FONT_H * 2];
    int row, col;
    for (row = 0; row < FONT_H; row++) {
        uint16_t bits = ((uint16_t)glyph[row * 2] << 8) | glyph[row * 2 + 1];
        for (col = 0; col < FONT_W; col++) {
            uint32_t c = (bits & (0x8000 >> col)) ? fg : bg;
            draw_px(s, x + col, y + row, c);
        }
    }
}

void draw_text(surface_t *s, int x, int y, const char *str,
               uint32_t fg, uint32_t bg)
{
    while (*str) {
        draw_char(s, x, y, *str, fg, bg);
        x += FONT_W;
        str++;
    }
}

void draw_text_t(surface_t *s, int x, int y, const char *str, uint32_t fg)
{
    while (*str) {
        const uint8_t *glyph = &font_terminus[(unsigned char)*str * FONT_H * 2];
        int row, col;
        for (row = 0; row < FONT_H; row++) {
            uint16_t bits = ((uint16_t)glyph[row * 2] << 8) | glyph[row * 2 + 1];
            for (col = 0; col < FONT_W; col++)
                if (bits & (0x8000 >> col))
                    draw_px(s, x + col, y + row, fg);
        }
        x += FONT_W;
        str++;
    }
}

void draw_blit(surface_t *dst, int dx, int dy,
               const uint32_t *src, int sw, int sh)
{
    int x, y;
    for (y = 0; y < sh; y++) {
        if (dy + y < 0 || dy + y >= dst->h)
            continue;
        for (x = 0; x < sw; x++) {
            if (dx + x < 0 || dx + x >= dst->w)
                continue;
            dst->buf[(dy + y) * dst->pitch + (dx + x)] = src[y * sw + x];
        }
    }
}
