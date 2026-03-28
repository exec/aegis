/* user/fb_test/main.c — Framebuffer test: draw a star on screen
 *
 * Maps the linear framebuffer via sys_fb_map (513), draws a white
 * 5-pointed star centered on the screen for 5 seconds, then exits.
 * The kernel's text terminal resumes on the next printk output.
 */
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <string.h>

/* Framebuffer info returned by sys_fb_map */
typedef struct {
    uint64_t addr;      /* user virtual address of mapped FB */
    uint32_t width;
    uint32_t height;
    uint32_t pitch;     /* bytes per scanline */
    uint32_t bpp;
} fb_info_t;

static void put_pixel(uint32_t *fb, uint32_t pitch_px,
                       int x, int y, uint32_t color)
{
    if (x >= 0 && y >= 0)
        fb[y * pitch_px + x] = color;
}

/* Draw a line from (x0,y0) to (x1,y1) using Bresenham's algorithm */
static void draw_line(uint32_t *fb, uint32_t pitch_px, uint32_t w, uint32_t h,
                      int x0, int y0, int x1, int y1, uint32_t color)
{
    int dx = x1 - x0;
    int dy = y1 - y0;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;

    while (1) {
        if (x0 >= 0 && x0 < (int)w && y0 >= 0 && y0 < (int)h)
            put_pixel(fb, pitch_px, x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

/* Draw a thick line (3px wide) */
static void draw_thick_line(uint32_t *fb, uint32_t pitch_px, uint32_t w, uint32_t h,
                            int x0, int y0, int x1, int y1, uint32_t color)
{
    draw_line(fb, pitch_px, w, h, x0, y0, x1, y1, color);
    draw_line(fb, pitch_px, w, h, x0+1, y0, x1+1, y1, color);
    draw_line(fb, pitch_px, w, h, x0, y0+1, x1, y1+1, color);
}

/* Fill a circle (filled disc) */
static void fill_circle(uint32_t *fb, uint32_t pitch_px, uint32_t w, uint32_t h,
                        int cx, int cy, int r, uint32_t color)
{
    int y;
    for (y = -r; y <= r; y++) {
        int x;
        for (x = -r; x <= r; x++) {
            if (x*x + y*y <= r*r) {
                int px = cx + x, py = cy + y;
                if (px >= 0 && px < (int)w && py >= 0 && py < (int)h)
                    put_pixel(fb, pitch_px, px, py, color);
            }
        }
    }
}

int main(void)
{
    fb_info_t info;
    memset(&info, 0, sizeof(info));

    long ret = syscall(513, &info);
    if (ret < 0 || info.addr == 0) {
        printf("fb_test: no framebuffer available (ret=%ld)\n", ret);
        return 1;
    }

    uint32_t *fb = (uint32_t *)(uintptr_t)info.addr;
    uint32_t w = info.width;
    uint32_t h = info.height;
    uint32_t pitch_px = info.pitch / 4;

    printf("fb_test: %ux%u framebuffer mapped at 0x%lx\n",
           w, h, (unsigned long)info.addr);

    /* Save first 64 pixels of FB to detect if it's real */
    uint32_t saved[64];
    memcpy(saved, fb, sizeof(saved));

    /* Clear screen to dark blue */
    uint32_t bg = 0x001428;  /* dark navy */
    uint32_t total = pitch_px * h;
    uint32_t i;
    for (i = 0; i < total; i++)
        fb[i] = bg;

    /* Draw a 5-pointed star centered on screen */
    int cx = (int)(w / 2);
    int cy = (int)(h / 2);
    int outer_r = (int)(h / 4);   /* outer radius */
    int inner_r = outer_r * 38 / 100; /* inner radius (~0.38 of outer) */

    /* Compute 10 points of the star (alternating outer/inner) */
    /* Using fixed-point sin/cos approximation to avoid libm */
    /* Pre-computed for 5-pointed star: angles 0, 36, 72, 108, ... degrees */
    /* sin/cos values × 1000 for the 10 angles (starting at -90° = top) */
    static const int sin10[10] = {
        -1000, -588, 309, 951, 809, 0, -809, -951, -309, 588
    };
    static const int cos10[10] = {
        0, 809, 951, 309, -588, -1000, -588, 309, 951, 809
    };

    int pts_x[10], pts_y[10];
    for (i = 0; i < 10; i++) {
        int r = (i % 2 == 0) ? outer_r : inner_r;
        pts_x[i] = cx + (r * cos10[i]) / 1000;
        pts_y[i] = cy + (r * sin10[i]) / 1000;
    }

    /* Draw star outline in bright gold */
    uint32_t gold = 0x00FFD700;
    for (i = 0; i < 10; i++) {
        int next = (i + 1) % 10;
        draw_thick_line(fb, pitch_px, w, h,
                        pts_x[i], pts_y[i],
                        pts_x[next], pts_y[next], gold);
    }

    /* Draw a small white circle in the center */
    fill_circle(fb, pitch_px, w, h, cx, cy, 8, 0x00FFFFFF);

    /* Draw title text using pixel art (simple block letters) */
    /* "AEGIS" centered below the star */
    int text_y = cy + outer_r + 40;
    int text_x = cx - 60;
    /* Just draw a line under the star as a simple indicator */
    draw_line(fb, pitch_px, w, h,
              text_x, text_y, text_x + 120, text_y, 0x00FFFFFF);
    draw_line(fb, pitch_px, w, h,
              text_x, text_y + 2, text_x + 120, text_y + 2, 0x00FFFFFF);

    printf("fb_test: star drawn! Waiting 5 seconds...\n");
    sleep(5);

    /* Clear back to black before returning to text mode */
    for (i = 0; i < total; i++)
        fb[i] = 0;

    printf("fb_test: done\n");
    return 0;
}
