/* about.c -- "About Aegis" information window for Lumen compositor.
 *
 * Displays system version, hardware info from /proc, and credits.
 * Renders as a frosted Glyph window with labels.
 */
#include "about.h"
#include <glyph.h>
#include <font.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

/* ---- System info helpers ---- */

static int
read_file_str(const char *path, char *buf, int bufsz)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    int n = (int)read(fd, buf, (size_t)(bufsz - 1));
    close(fd);
    if (n < 0) return -1;
    buf[n] = '\0';
    /* Strip trailing whitespace */
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r' || buf[n-1] == ' '))
        buf[--n] = '\0';
    return n;
}

/* Parse "MemTotal: XXXXX kB" from /proc/meminfo */
static void
get_mem_total(char *out, int outsz)
{
    char buf[512];
    if (read_file_str("/proc/meminfo", buf, sizeof(buf)) <= 0) {
        snprintf(out, outsz, "Unknown");
        return;
    }
    char *p = strstr(buf, "MemTotal:");
    if (!p) { snprintf(out, outsz, "Unknown"); return; }
    p += 9;
    while (*p == ' ') p++;
    /* Extract number */
    long kb = 0;
    while (*p >= '0' && *p <= '9') {
        kb = kb * 10 + (*p - '0');
        p++;
    }
    if (kb > 1024)
        snprintf(out, outsz, "%ld MB", kb / 1024);
    else
        snprintf(out, outsz, "%ld kB", kb);
}

/* Count CPUs from /proc/meminfo (look for "CPUs:" line) or fallback */
static void
get_cpu_info(char *out, int outsz)
{
    /* /proc doesn't have cpuinfo yet — read from SMP init message */
    char buf[512];
    if (read_file_str("/proc/meminfo", buf, sizeof(buf)) > 0) {
        char *p = strstr(buf, "CPUs:");
        if (p) {
            p += 5;
            while (*p == ' ') p++;
            int n = 0;
            while (*p >= '0' && *p <= '9') {
                n = n * 10 + (*p - '0');
                p++;
            }
            if (n > 0) {
                snprintf(out, outsz, "%d core%s", n, n > 1 ? "s" : "");
                return;
            }
        }
    }
    snprintf(out, outsz, "x86_64");
}

/* ---- Logo loading (reuse Bastion's format) ---- */

static uint32_t *s_logo_px;
static int s_logo_w, s_logo_h;

static void
load_logo(void)
{
    int fd = open("/usr/share/logo.raw", O_RDONLY);
    if (fd < 0) return;
    uint32_t hdr[2];
    if (read(fd, hdr, 8) != 8) { close(fd); return; }
    s_logo_w = (int)hdr[0];
    s_logo_h = (int)hdr[1];
    if (s_logo_w <= 0 || s_logo_h <= 0 || s_logo_w > 1200 || s_logo_h > 600) {
        close(fd); s_logo_w = s_logo_h = 0; return;
    }
    size_t sz = (size_t)(s_logo_w * s_logo_h) * 4;
    s_logo_px = malloc(sz);
    if (!s_logo_px) { close(fd); s_logo_w = s_logo_h = 0; return; }
    size_t got = 0;
    while (got < sz) {
        ssize_t n = read(fd, (char *)s_logo_px + got, sz - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(fd);
    if (got < sz) { free(s_logo_px); s_logo_px = NULL; s_logo_w = s_logo_h = 0; }
}

/* ---- Claude logo loading ---- */

static uint32_t *s_claude_px;
static int s_claude_w, s_claude_h;

static int s_screen_w, s_screen_h;  /* actual display resolution */

static void
load_claude_logo(void)
{
    int fd = open("/usr/share/claude.raw", O_RDONLY);
    if (fd < 0) return;
    uint32_t hdr[2];
    if (read(fd, hdr, 8) != 8) { close(fd); return; }
    s_claude_w = (int)hdr[0];
    s_claude_h = (int)hdr[1];
    if (s_claude_w <= 0 || s_claude_h <= 0 || s_claude_w > 400 || s_claude_h > 400) {
        close(fd); s_claude_w = s_claude_h = 0; return;
    }
    size_t sz = (size_t)(s_claude_w * s_claude_h) * 4;
    s_claude_px = malloc(sz);
    if (!s_claude_px) { close(fd); s_claude_w = s_claude_h = 0; return; }
    size_t got = 0;
    while (got < sz) {
        ssize_t n = read(fd, (char *)s_claude_px + got, sz - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(fd);
    if (got < sz) { free(s_claude_px); s_claude_px = NULL; s_claude_w = s_claude_h = 0; }
}

/* ---- Custom render for About window ---- */

static void
about_render(glyph_window_t *win)
{
    surface_t *s = &win->surface;
    int cx = GLYPH_BORDER_WIDTH;
    int cy = GLYPH_BORDER_WIDTH + GLYPH_TITLEBAR_HEIGHT;
    int cw = win->client_w;

    /* Fill client area with key color for frosted transparency */
    draw_fill_rect(s, cx, cy, cw, win->client_h, C_SHADOW);

    int y = cy + 16;

    /* Aegis logo (scaled to 25% — ~218x56) */
    if (s_logo_px && s_logo_w > 0) {
        int dw = s_logo_w / 4;
        int dh = s_logo_h / 4;
        int lx = cx + (cw - dw) / 2;
        for (int row = 0; row < dh; row++) {
            int sr = row * 4;
            if (sr >= s_logo_h) break;
            for (int col = 0; col < dw; col++) {
                int sc = col * 4;
                if (sc >= s_logo_w) break;
                uint32_t px = s_logo_px[sr * s_logo_w + sc];
                uint32_t a = (px >> 24) & 0xFF;
                if (a > 0) {
                    int dx = lx + col;
                    int dy = y + row;
                    if (dx >= 0 && dx < s->w && dy >= 0 && dy < s->h) {
                        if (a == 0xFF) {
                            s->buf[dy * s->pitch + dx] = px & 0x00FFFFFF;
                        } else {
                            uint32_t bg = s->buf[dy * s->pitch + dx];
                            uint32_t inv = 255 - a;
                            uint32_t r = (((px >> 16) & 0xFF) * a + ((bg >> 16) & 0xFF) * inv) / 255;
                            uint32_t g = (((px >> 8) & 0xFF) * a + ((bg >> 8) & 0xFF) * inv) / 255;
                            uint32_t b = ((px & 0xFF) * a + (bg & 0xFF) * inv) / 255;
                            s->buf[dy * s->pitch + dx] = (r << 16) | (g << 8) | b;
                        }
                    }
                }
            }
        }
        y += dh + 12;
    }

    /* Version */
    if (g_font_ui) {
        const char *ver = "Version 1.0.0  \"Ambient Argus\"";
        int tw = font_text_width(g_font_ui, 16, ver);
        font_draw_text(s, g_font_ui, 16, cx + (cw - tw) / 2, y, ver, 0x00B0B0C0);
    } else {
        draw_text_t(s, cx + 20, y, "Version 1.0.0", 0x00B0B0C0);
    }
    y += 24;

    /* Tagline */
    if (g_font_ui) {
        const char *tag = "Capability-based POSIX-compatible kernel";
        int tw = font_text_width(g_font_ui, 14, tag);
        font_draw_text(s, g_font_ui, 14, cx + (cw - tw) / 2, y, tag, 0x00808898);
    }
    y += 30;

    /* Separator */
    draw_blend_rect(s, cx + 30, y, cw - 60, 1, 0x00FFFFFF, 20);
    y += 16;

    /* System info */
    char mem_str[64], cpu_str[64];
    get_mem_total(mem_str, sizeof(mem_str));
    get_cpu_info(cpu_str, sizeof(cpu_str));

    char info[128];
    snprintf(info, sizeof(info), "CPU: %s", cpu_str);
    if (g_font_ui)
        font_draw_text(s, g_font_ui, 14, cx + 24, y, info, C_TEXT);
    y += 22;

    snprintf(info, sizeof(info), "Memory: %s", mem_str);
    if (g_font_ui)
        font_draw_text(s, g_font_ui, 14, cx + 24, y, info, C_TEXT);
    y += 22;

    snprintf(info, sizeof(info), "Display: %dx%d", s_screen_w, s_screen_h);
    if (g_font_ui)
        font_draw_text(s, g_font_ui, 14, cx + 24, y, info, C_TEXT);
    y += 30;

    /* Separator */
    draw_blend_rect(s, cx + 30, y, cw - 60, 1, 0x00FFFFFF, 20);
    y += 16;

    /* Credits */
    if (g_font_ui) {
        font_draw_text(s, g_font_ui, 14, cx + 24, y,
                       "Created by Dylan Hart", 0x00C0C0D0);
        y += 20;
        font_draw_text(s, g_font_ui, 13, cx + 24, y,
                       "github.com/exec", 0x00808898);
        y += 28;
    } else {
        draw_text_t(s, cx + 24, y, "Created by Dylan Hart", 0x00C0C0D0);
        y += 20;
        draw_text_t(s, cx + 24, y, "github.com/exec", 0x00808898);
        y += 24;
    }

    /* Claude logo + attribution — pinned to bottom of window */
    {
        int target_h = 24;
        int by = cy + win->client_h - target_h - 12;  /* 12px from bottom */

        if (s_claude_px && s_claude_w > 0) {
            int target_w = s_claude_w * target_h / s_claude_h;
            if (target_w <= 0) target_w = 24;

            /* Center the logo+text as a unit */
            int text_w = g_font_ui ? font_text_width(g_font_ui, 13, "Built with Claude Code") : 180;
            int total_unit = target_w + 8 + text_w;
            int lx = cx + (cw - total_unit) / 2;

            for (int row = 0; row < target_h; row++) {
                int sr = row * s_claude_h / target_h;
                if (sr >= s_claude_h) break;
                for (int col = 0; col < target_w; col++) {
                    int sc = col * s_claude_w / target_w;
                    if (sc >= s_claude_w) break;
                    uint32_t px = s_claude_px[sr * s_claude_w + sc];
                    uint32_t a = (px >> 24) & 0xFF;
                    if (a > 0) {
                        int dx = lx + col;
                        int dy = by + row;
                        if (dx >= 0 && dx < s->w && dy >= 0 && dy < s->h) {
                            if (a == 0xFF) {
                                s->buf[dy * s->pitch + dx] = px & 0x00FFFFFF;
                            } else {
                                uint32_t bg_px = s->buf[dy * s->pitch + dx];
                                uint32_t inv = 255 - a;
                                uint32_t r2 = (((px >> 16) & 0xFF) * a + ((bg_px >> 16) & 0xFF) * inv) / 255;
                                uint32_t g2 = (((px >> 8) & 0xFF) * a + ((bg_px >> 8) & 0xFF) * inv) / 255;
                                uint32_t b2 = ((px & 0xFF) * a + (bg_px & 0xFF) * inv) / 255;
                                s->buf[dy * s->pitch + dx] = (r2 << 16) | (g2 << 8) | b2;
                            }
                        }
                    }
                }
            }
            if (g_font_ui) {
                int tx = lx + target_w + 8;
                int ty = by + (target_h - font_height(g_font_ui, 13)) / 2;
                font_draw_text(s, g_font_ui, 13, tx, ty,
                               "Built with Claude Code", 0x00808898);
            }
        } else {
            if (g_font_ui) {
                const char *cc = "Built with Claude Code";
                int tw = font_text_width(g_font_ui, 13, cc);
                font_draw_text(s, g_font_ui, 13, cx + (cw - tw) / 2, by,
                               cc, 0x00808898);
            }
        }
    }
}

/* ---- Public API ---- */

glyph_window_t *
about_create(int screen_w, int screen_h)
{
    s_screen_w = screen_w;
    s_screen_h = screen_h;

    /* Load logos on first call */
    if (!s_logo_px)
        load_logo();
    if (!s_claude_px)
        load_claude_logo();

    glyph_window_t *win = glyph_window_create("About Aegis", 380, 400);
    if (!win) return NULL;

    win->on_render = about_render;
    win->x = (screen_w - win->surf_w) / 2;
    win->y = 80;

    glyph_window_mark_all_dirty(win);
    return win;
}
