/* citadel-dock — standalone dock binary that talks to Lumen via the
 * external window protocol (LUMEN_OP_CREATE_PANEL + LUMEN_OP_INVOKE).
 *
 * Splits the dock out of libcitadel (which used to be linked into Lumen
 * itself and rendered as an overlay callback). The protocol can't read
 * underlying compositor pixels, so the frosted-glass blur is dropped:
 * we render an opaque dark background instead. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <stdint.h>

#include <glyph.h>
#include <font.h>
#include <lumen_client.h>

/* Dock geometry — must match the old dock.h numbers so the existing
 * dock_click_test geometry remains valid. */
#define DOCK_ICON_SIZE   48
#define DOCK_ICON_GAP    12
#define DOCK_PADDING_X   24
#define DOCK_PADDING_Y   16
#define DOCK_HEIGHT      (DOCK_ICON_SIZE + DOCK_PADDING_Y * 2)
#define DOCK_BG          0x002A2A3E
#define DOCK_HOVER_BG    0x00FFFFFF
#define DOCK_HOVER_ALPHA 40
#define DOCK_GLASS_TINT  0x001A1A2E

#define DOCK_ITEM_SETTINGS  0
#define DOCK_ITEM_FILES     1
#define DOCK_ITEM_TERMINAL  2
#define DOCK_ITEM_WIDGETS   3
#define DOCK_ITEM_COUNT     4

static const char *s_item_keys[DOCK_ITEM_COUNT] = {
    [DOCK_ITEM_SETTINGS] = "settings",
    [DOCK_ITEM_FILES]    = "files",
    [DOCK_ITEM_TERMINAL] = "terminal",
    [DOCK_ITEM_WIDGETS]  = "widgets",
};

static int s_dock_w;
static int s_hover = -1;

static void
log_console(const char *msg)
{
    write(2, msg, strlen(msg));
    int cfd = open("/dev/console", O_WRONLY);
    if (cfd >= 0) { write(cfd, msg, strlen(msg)); close(cfd); }
}

/* Bridge surface_t expected by glyph draw_* over the lumen window backbuf. */
static surface_t
backbuf_surface(lumen_window_t *win)
{
    surface_t s;
    s.buf   = (uint32_t *)win->backbuf;
    s.w     = win->w;
    s.h     = win->h;
    s.pitch = win->stride;
    return s;
}

/* Local-coord rect of icon i within the dock surface. */
static void
item_rect(int i, int *ix, int *iy)
{
    *ix = DOCK_PADDING_X + i * (DOCK_ICON_SIZE + DOCK_ICON_GAP);
    *iy = DOCK_PADDING_Y;
}

/* ── Icon renderers (ported from libcitadel/dock.c) ─────────────────── */

static void
draw_icon_settings(surface_t *fb, int cx, int cy)
{
    draw_circle_filled(fb, cx, cy, 18, C_ACCENT);
    int tw = 8, th = 6;
    draw_fill_rect(fb, cx - tw / 2, cy - 22, tw, th, C_ACCENT);
    draw_fill_rect(fb, cx - tw / 2, cy + 16, tw, th, C_ACCENT);
    draw_fill_rect(fb, cx - 22, cy - th / 2, th, tw, C_ACCENT);
    draw_fill_rect(fb, cx + 16, cy - th / 2, th, tw, C_ACCENT);
    int d = 14;
    draw_fill_rect(fb, cx + d - 3, cy - d - 3, 6, 6, C_ACCENT);
    draw_fill_rect(fb, cx - d - 3, cy - d - 3, 6, 6, C_ACCENT);
    draw_fill_rect(fb, cx + d - 3, cy + d - 3, 6, 6, C_ACCENT);
    draw_fill_rect(fb, cx - d - 3, cy + d - 3, 6, 6, C_ACCENT);
    draw_circle_filled(fb, cx, cy, 8, DOCK_GLASS_TINT);
}

static void
draw_icon_folder(surface_t *fb, int ix, int iy)
{
    int fw = 40, fh = 28;
    int fx = ix + (DOCK_ICON_SIZE - fw) / 2;
    int fy = iy + (DOCK_ICON_SIZE - fh) / 2 + 2;
    draw_rounded_rect(fb, fx, fy - 6, 16, 8, 3, 0x00CC9944);
    draw_rounded_rect(fb, fx, fy, fw, fh, 4, 0x00CC9944);
    draw_fill_rect(fb, fx + 2, fy + 2, fw - 4, 3, 0x00BB8833);
}

static void
draw_icon_terminal(surface_t *fb, int ix, int iy)
{
    int tw = 42, th = 32;
    int tx = ix + (DOCK_ICON_SIZE - tw) / 2;
    int ty = iy + (DOCK_ICON_SIZE - th) / 2;
    draw_rounded_rect(fb, tx, ty, tw, th, 6, 0x001A1A2E);
    if (g_font_ui) {
        int tw2 = font_text_width(g_font_ui, 14, ">_");
        int text_x = tx + (tw - tw2) / 2;
        int text_y = ty + (th - font_height(g_font_ui, 14)) / 2;
        font_draw_text(fb, g_font_ui, 14, text_x, text_y, ">_", C_TERM_FG);
    } else {
        int text_x = tx + (tw - 3 * FONT_W) / 2;
        int text_y = ty + (th - FONT_H) / 2;
        draw_text(fb, text_x, text_y, ">_", C_TERM_FG, 0x001A1A2E);
    }
}

static void
draw_icon_widgets(surface_t *fb, int ix, int iy)
{
    int bw = 38, bh = 30;
    int bx = ix + (DOCK_ICON_SIZE - bw) / 2;
    int by = iy + (DOCK_ICON_SIZE - bh) / 2;
    int cell = 14, gap = 4;
    draw_rounded_rect(fb, bx, by, cell, cell, 3, 0x004488CC);
    draw_rounded_rect(fb, bx + cell + gap, by, cell, cell, 3, 0x0050FA7B);
    draw_rounded_rect(fb, bx, by + cell + gap, cell, cell, 3, 0x00FF7744);
    draw_rounded_rect(fb, bx + cell + gap, by + cell + gap, cell, cell, 3, 0x00CC66FF);
}

static void
render_dock(lumen_window_t *win)
{
    surface_t s = backbuf_surface(win);

    /* Opaque dark background. The old in-process dock blurred the
     * compositor backbuf; from a separate process we only see our own
     * buffer, so we draw flat. Compositor will blit this opaquely. */
    draw_fill_rect(&s, 0, 0, s.w, s.h, DOCK_BG);

    for (int i = 0; i < DOCK_ITEM_COUNT; i++) {
        int ix, iy;
        item_rect(i, &ix, &iy);
        if (i == s_hover) {
            draw_blend_rect(&s, ix - 4, iy - 4,
                            DOCK_ICON_SIZE + 8, DOCK_ICON_SIZE + 8,
                            DOCK_HOVER_BG, DOCK_HOVER_ALPHA);
        }
        switch (i) {
        case DOCK_ITEM_SETTINGS:
            draw_icon_settings(&s, ix + DOCK_ICON_SIZE / 2,
                               iy + DOCK_ICON_SIZE / 2);
            break;
        case DOCK_ITEM_FILES:    draw_icon_folder(&s, ix, iy);   break;
        case DOCK_ITEM_TERMINAL: draw_icon_terminal(&s, ix, iy); break;
        case DOCK_ITEM_WIDGETS:  draw_icon_widgets(&s, ix, iy);  break;
        }
    }
}

static int
hit_test(int lx, int ly)
{
    if (lx < 0 || lx >= s_dock_w || ly < 0 || ly >= DOCK_HEIGHT)
        return -1;
    for (int i = 0; i < DOCK_ITEM_COUNT; i++) {
        int ix, iy;
        item_rect(i, &ix, &iy);
        if (lx >= ix && lx < ix + DOCK_ICON_SIZE &&
            ly >= iy && ly < iy + DOCK_ICON_SIZE)
            return i;
    }
    return -1;
}

/* Emit the same [DOCK] debug lines the old in-process dock did so that
 * dock_click_test (which parses serial for icon centers) keeps working. */
static void
emit_debug_lines(int dock_screen_x, int dock_screen_y)
{
    char buf[160];
    for (int i = 0; i < DOCK_ITEM_COUNT; i++) {
        int ix, iy;
        item_rect(i, &ix, &iy);
        int cx = dock_screen_x + ix + DOCK_ICON_SIZE / 2;
        int cy = dock_screen_y + iy + DOCK_ICON_SIZE / 2;
        int hw = DOCK_ICON_SIZE / 2;
        int hh = DOCK_ICON_SIZE / 2;
        int n = snprintf(buf, sizeof(buf),
                         "[DOCK] item=%s idx=%d cx=%d cy=%d hw=%d hh=%d\n",
                         s_item_keys[i], i, cx, cy, hw, hh);
        if (n > 0) log_console(buf);
    }
    log_console("[DOCK] ready\n");
}

int main(void)
{
    /* Wait forever for Lumen. Vigil starts citadel-dock at boot under
     * graphical mode, but Lumen doesn't bind /run/lumen.sock until
     * Bastion authenticates the user — which can be any number of
     * minutes later. Exiting on timeout would burn vigil restart credits
     * for no reason. Loop indefinitely instead. */
    int fd = -1;
    for (;;) {
        fd = lumen_connect();
        if (fd >= 0) break;
        if (fd != -111) {  /* ECONNREFUSED is expected; anything else is fatal */
            char buf[96];
            int n = snprintf(buf, sizeof(buf),
                "[DOCK] lumen_connect=%d (giving up)\n", fd);
            if (n > 0) log_console(buf);
            return 1;
        }
        struct timespec ts = { 0, 200 * 1000 * 1000 };  /* 200ms */
        nanosleep(&ts, NULL);
    }

    /* Initialize TTF font renderer so terminal icon ">_" looks right. */
    font_init();

    s_dock_w = DOCK_PADDING_X * 2 +
               DOCK_ITEM_COUNT * DOCK_ICON_SIZE +
               (DOCK_ITEM_COUNT - 1) * DOCK_ICON_GAP;

    lumen_window_t *win = lumen_panel_create(fd, s_dock_w, DOCK_HEIGHT);
    if (!win) {
        log_console("[DOCK] FAIL: panel_create returned NULL\n");
        close(fd);
        return 1;
    }

    /* dock_click_test parses [DOCK] lines for icon SCREEN-centers.
     * lumen_window_created_t reply now carries the panel's screen
     * position (Lumen places panels at bottom-center). */
    emit_debug_lines(win->x, win->y);

    render_dock(win);
    lumen_window_present(win);

    /* Event loop */
    for (;;) {
        lumen_event_t ev;
        int r = lumen_wait_event(fd, &ev, -1);  /* block forever */
        if (r < 0) {
            log_console("[DOCK] connection lost\n");
            break;
        }
        if (r == 0) continue;

        switch (ev.type) {
        case LUMEN_EV_MOUSE: {
            int item = hit_test(ev.mouse.x, ev.mouse.y);
            if (ev.mouse.evtype == LUMEN_MOUSE_MOVE) {
                if (item != s_hover) {
                    s_hover = item;
                    render_dock(win);
                    lumen_window_present(win);
                }
            } else if (ev.mouse.evtype == LUMEN_MOUSE_DOWN &&
                       (ev.mouse.buttons & 1)) {
                if (item == DOCK_ITEM_TERMINAL) {
                    lumen_invoke(fd, "terminal");
                } else if (item == DOCK_ITEM_WIDGETS) {
                    lumen_invoke(fd, "widgets");
                }
                /* settings / files: no-op stubs */
            }
            break;
        }
        case LUMEN_EV_CLOSE_REQUEST:
            /* Ignore — dock is unkillable from the UI. */
            break;
        default:
            break;
        }
    }

    lumen_window_destroy(win);
    close(fd);
    return 0;
}
