/* draw.c — Framebuffer drawing primitives for Lumen compositor
 *
 * All functions take a surface_t pointer and perform bounds-checked
 * pixel writes. Font rendering uses the Terminus 10x20 bitmap font.
 */
#include "draw.h"
#include "terminus20.h"
#include <stdlib.h>
#include <string.h>

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

void draw_line(surface_t *s, int x0, int y0, int x1, int y1, uint32_t color)
{
    int dx = x1 - x0;
    int dy = y1 - y0;
    int sx = dx >= 0 ? 1 : -1;
    int sy = dy >= 0 ? 1 : -1;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    int err = dx - dy;

    for (;;) {
        draw_px(s, x0, y0, color);
        if (x0 == x1 && y0 == y1)
            break;
        int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void draw_circle(surface_t *s, int cx, int cy, int r, uint32_t color)
{
    int x = r, y = 0;
    int d = 1 - r;

    while (x >= y) {
        draw_px(s, cx + x, cy + y, color);
        draw_px(s, cx - x, cy + y, color);
        draw_px(s, cx + x, cy - y, color);
        draw_px(s, cx - x, cy - y, color);
        draw_px(s, cx + y, cy + x, color);
        draw_px(s, cx - y, cy + x, color);
        draw_px(s, cx + y, cy - x, color);
        draw_px(s, cx - y, cy - x, color);
        y++;
        if (d <= 0) {
            d += 2 * y + 1;
        } else {
            x--;
            d += 2 * (y - x) + 1;
        }
    }
}

static void draw_hline(surface_t *s, int x0, int x1, int y, uint32_t color)
{
    int i;
    if (y < 0 || y >= s->h)
        return;
    if (x0 > x1) {
        int tmp = x0; x0 = x1; x1 = tmp;
    }
    if (x0 < 0) x0 = 0;
    if (x1 >= s->w) x1 = s->w - 1;
    for (i = x0; i <= x1; i++)
        s->buf[y * s->pitch + i] = color;
}

void draw_circle_filled(surface_t *s, int cx, int cy, int r, uint32_t color)
{
    int x = r, y = 0;
    int d = 1 - r;

    while (x >= y) {
        draw_hline(s, cx - x, cx + x, cy + y, color);
        draw_hline(s, cx - x, cx + x, cy - y, color);
        draw_hline(s, cx - y, cx + y, cy + x, color);
        draw_hline(s, cx - y, cx + y, cy - x, color);
        y++;
        if (d <= 0) {
            d += 2 * y + 1;
        } else {
            x--;
            d += 2 * (y - x) + 1;
        }
    }
}

void draw_rounded_rect(surface_t *s, int x, int y, int w, int h,
                       int r, uint32_t color)
{
    int i;
    if (r < 0) r = 0;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;

    /* Center rectangle */
    draw_fill_rect(s, x + r, y, w - 2 * r, h, color);
    /* Left strip */
    draw_fill_rect(s, x, y + r, r, h - 2 * r, color);
    /* Right strip */
    draw_fill_rect(s, x + w - r, y + r, r, h - 2 * r, color);

    /* Four corner quarter-circles */
    {
        int cx_tl = x + r, cy_tl = y + r;
        int cx_tr = x + w - r - 1, cy_tr = y + r;
        int cx_bl = x + r, cy_bl = y + h - r - 1;
        int cx_br = x + w - r - 1, cy_br = y + h - r - 1;
        int px = r, py = 0;
        int d = 1 - r;

        while (px >= py) {
            /* Top-left corner */
            draw_hline(s, cx_tl - px, cx_tl, cy_tl - py, color);
            draw_hline(s, cx_tl - py, cx_tl, cy_tl - px, color);
            /* Top-right corner */
            draw_hline(s, cx_tr, cx_tr + px, cy_tr - py, color);
            draw_hline(s, cx_tr, cx_tr + py, cy_tr - px, color);
            /* Bottom-left corner */
            draw_hline(s, cx_bl - px, cx_bl, cy_bl + py, color);
            draw_hline(s, cx_bl - py, cx_bl, cy_bl + px, color);
            /* Bottom-right corner */
            draw_hline(s, cx_br, cx_br + px, cy_br + py, color);
            draw_hline(s, cx_br, cx_br + py, cy_br + px, color);

            py++;
            if (d <= 0) {
                d += 2 * py + 1;
            } else {
                px--;
                d += 2 * (py - px) + 1;
            }
        }
    }
}

void draw_blit_scaled(surface_t *dst, int dx, int dy, int dw, int dh,
                      const uint32_t *src, int sw, int sh)
{
    int y;
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0)
        return;

    /* Fixed-point 16.16 stepping — eliminates per-pixel division in inner loop */
    int x_step = (sw << 16) / dw;

    /* Clamp destination to surface bounds for row-level skip */
    int y0 = 0, y1 = dh;
    if (dy < 0) y0 = -dy;
    if (dy + dh > dst->h) y1 = dst->h - dy;
    int x0 = 0, x1 = dw;
    if (dx < 0) x0 = -dx;
    if (dx + dw > dst->w) x1 = dst->w - dx;

    for (y = y0; y < y1; y++) {
        int src_y = y * sh / dh;
        const uint32_t *src_row = src + src_y * sw;
        uint32_t *dst_row = dst->buf + (dy + y) * dst->pitch + dx;
        int x_acc = x0 * x_step;
        for (int x = x0; x < x1; x++) {
            dst_row[x] = src_row[x_acc >> 16];
            x_acc += x_step;
        }
    }
}

void draw_text_center(surface_t *s, int x, int y, int w, const char *str,
                      uint32_t fg, uint32_t bg)
{
    int len = 0;
    const char *p = str;
    while (*p++) len++;
    int text_w = len * FONT_W;
    int offset = (w - text_w) / 2;
    if (offset < 0) offset = 0;
    draw_text(s, x + offset, y, str, fg, bg);
}

void draw_box_blur(surface_t *s, int x, int y, int w, int h, int radius)
{
    /* Clamp region to surface bounds */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > s->w) w = s->w - x;
    if (y + h > s->h) h = s->h - y;
    if (w <= 0 || h <= 0 || radius <= 0)
        return;

    int span = 2 * radius + 1;

    /* Scratch buffer for one channel row/column — max of w or h */
    int scratch_len = w > h ? w : h;
    uint16_t *tmp = (uint16_t *)malloc((size_t)scratch_len * sizeof(uint16_t));
    if (!tmp)
        return;

    /* Horizontal pass — blur each row */
    for (int row = y; row < y + h; row++) {
        uint32_t *rowp = &s->buf[row * s->pitch];

        /* Process R, G, B channels separately */
        for (int ch = 0; ch < 3; ch++) {
            int shift = ch * 8;

            /* Extract channel into tmp */
            for (int i = 0; i < w; i++)
                tmp[i] = (uint16_t)((rowp[x + i] >> shift) & 0xFF);

            /* Running sum with clamped edges */
            int sum = 0;
            for (int i = -radius; i <= radius; i++) {
                int idx = i < 0 ? 0 : (i >= w ? w - 1 : i);
                sum += tmp[idx];
            }
            uint16_t first_val = tmp[0];
            uint16_t last_val = tmp[w - 1];

            for (int i = 0; i < w; i++) {
                uint8_t avg = (uint8_t)(sum / span);
                /* Write averaged channel back */
                rowp[x + i] = (rowp[x + i] & ~(0xFFu << shift)) |
                               ((uint32_t)avg << shift);

                /* Slide the window: remove left edge, add right edge */
                int old_idx = i - radius;
                int new_idx = i + radius + 1;
                int old_val = old_idx < 0 ? first_val : tmp[old_idx];
                int new_val = new_idx >= w ? last_val : tmp[new_idx];
                sum += new_val - old_val;
            }
        }
    }

    /* Vertical pass — blur each column */
    for (int col = x; col < x + w; col++) {
        for (int ch = 0; ch < 3; ch++) {
            int shift = ch * 8;

            /* Extract channel into tmp */
            for (int i = 0; i < h; i++)
                tmp[i] = (uint16_t)((s->buf[(y + i) * s->pitch + col] >> shift) & 0xFF);

            /* Running sum with clamped edges */
            int sum = 0;
            for (int i = -radius; i <= radius; i++) {
                int idx = i < 0 ? 0 : (i >= h ? h - 1 : i);
                sum += tmp[idx];
            }
            uint16_t first_val = tmp[0];
            uint16_t last_val = tmp[h - 1];

            for (int i = 0; i < h; i++) {
                uint8_t avg = (uint8_t)(sum / span);
                s->buf[(y + i) * s->pitch + col] =
                    (s->buf[(y + i) * s->pitch + col] & ~(0xFFu << shift)) |
                    ((uint32_t)avg << shift);

                int old_idx = i - radius;
                int new_idx = i + radius + 1;
                int old_val = old_idx < 0 ? first_val : tmp[old_idx];
                int new_val = new_idx >= h ? last_val : tmp[new_idx];
                sum += new_val - old_val;
            }
        }
    }

    free(tmp);
}

void draw_blit_keyed(surface_t *dst, int dx, int dy,
                     const uint32_t *src, int sw, int sh, uint32_t key_color)
{
    for (int y = 0; y < sh; y++) {
        if (dy + y < 0 || dy + y >= dst->h)
            continue;
        for (int x = 0; x < sw; x++) {
            if (dx + x < 0 || dx + x >= dst->w)
                continue;
            uint32_t px = src[y * sw + x];
            if (px != key_color)
                dst->buf[(dy + y) * dst->pitch + (dx + x)] = px;
        }
    }
}

void draw_blend_rect(surface_t *s, int x, int y, int w, int h,
                     uint32_t color, int alpha)
{
    /* Clamp region to surface bounds */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > s->w) w = s->w - x;
    if (y + h > s->h) h = s->h - y;
    if (w <= 0 || h <= 0)
        return;

    if (alpha < 0) alpha = 0;
    if (alpha > 255) alpha = 255;
    int inv_alpha = 255 - alpha;

    uint32_t cr = (color >> 16) & 0xFF;
    uint32_t cg = (color >> 8) & 0xFF;
    uint32_t cb = color & 0xFF;

    for (int j = y; j < y + h; j++) {
        uint32_t *rowp = &s->buf[j * s->pitch];
        for (int i = x; i < x + w; i++) {
            uint32_t px = rowp[i];
            uint32_t pr = (px >> 16) & 0xFF;
            uint32_t pg = (px >> 8) & 0xFF;
            uint32_t pb = px & 0xFF;

            uint32_t or_ = (cr * (uint32_t)alpha + pr * (uint32_t)inv_alpha) / 255;
            uint32_t og = (cg * (uint32_t)alpha + pg * (uint32_t)inv_alpha) / 255;
            uint32_t ob = (cb * (uint32_t)alpha + pb * (uint32_t)inv_alpha) / 255;

            rowp[i] = (or_ << 16) | (og << 8) | ob;
        }
    }
}
