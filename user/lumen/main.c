/* main.c -- Lumen compositor entry point and event loop */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <time.h>
#include <sys/syscall.h>

#include <glyph.h>
#include "cursor.h"
#include "compositor.h"
#include "terminal.h"

typedef struct {
    uint64_t addr;
    uint32_t width, height, pitch, bpp;
} fb_info_t;

typedef struct __attribute__((packed)) {
    uint8_t  buttons;
    int16_t  dx;
    int16_t  dy;
    int16_t  scroll;
} mouse_event_t;

#define INFO_BG 0x00F0F4F8

static struct termios s_orig_termios;

static void
restore_terminal(void)
{
    tcsetattr(0, TCSANOW, &s_orig_termios);
}

/* Store fb dimensions for info window render callback */
static int s_fb_w, s_fb_h;

static void
info_render(glyph_window_t *win)
{
    surface_t *s = &win->surface;
    int ox = GLYPH_BORDER_WIDTH;
    int oy = GLYPH_BORDER_WIDTH + GLYPH_TITLEBAR_HEIGHT;
    int cw = win->client_w;
    int ch = win->client_h;

    draw_fill_rect(s, ox, oy, cw, ch, INFO_BG);

    int y = oy + 8;
    int x = ox + 10;
    int lh = FONT_H + 2;
    char buf[80];

    draw_text(s, x, y, "SYSTEM", C_ACCENT, INFO_BG);
    y += lh + 4;

    draw_text(s, x, y, "Kernel:   Aegis 0.1 (x86_64)", C_TEXT, INFO_BG);
    y += lh;
    draw_text(s, x, y, "Arch:     x86_64 (AMD64)", C_TEXT, INFO_BG);
    y += lh;
    snprintf(buf, sizeof(buf), "Display:  %ux%u @ 32bpp", s_fb_w, s_fb_h);
    draw_text(s, x, y, buf, C_TEXT, INFO_BG);
    y += lh;
    draw_text(s, x, y, "Security: Capability-based", C_TEXT, INFO_BG);
    y += lh;
    draw_text(s, x, y, "Shell:    oksh (OpenBSD ksh)", C_TEXT, INFO_BG);
    y += lh;
    draw_text(s, x, y, "Init:     Vigil supervisor", C_TEXT, INFO_BG);
}

static glyph_window_t *
create_info_window(int fb_w, int fb_h)
{
    int cols = 35;
    int rows = 10;
    int cw = cols * FONT_W;
    int ch = rows * FONT_H;

    s_fb_w = fb_w;
    s_fb_h = fb_h;

    glyph_window_t *win = glyph_window_create("System", cw, ch);
    if (!win)
        return NULL;

    win->x = 20;
    win->y = 20;
    win->on_render = info_render;
    win->priv = (void *)1;  /* non-NULL so chrome skips client fill */

    return win;
}

int
main(void)
{
    fb_info_t fb_info;
    memset(&fb_info, 0, sizeof(fb_info));

    /* Map framebuffer via sys_fb_map (513) */
    long ret = syscall(513, &fb_info);
    if (ret < 0) {
        write(2, "lumen: fb_map failed\n", 20);
        return 1;
    }
    write(2, "lumen: fb ok\n", 13);

    uint32_t *fb = (uint32_t *)(uintptr_t)fb_info.addr;
    int fb_w = (int)fb_info.width;
    int fb_h = (int)fb_info.height;
    int pitch_px = (int)(fb_info.pitch / (fb_info.bpp / 8));

    /* Allocate backbuffer */
    uint32_t *backbuf = malloc((size_t)pitch_px * fb_h * 4);
    if (!backbuf) {
        write(2, "lumen: malloc fail\n", 19);
        return 1;
    }
    write(2, "lumen: backbuf ok\n", 18);

    /* Set stdin to raw mode: no echo, no canonical, no signals */
    tcgetattr(0, &s_orig_termios);
    atexit(restore_terminal);
    struct termios raw = s_orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(0, TCSANOW, &raw);

    /* Open mouse device */
    int mouse_fd = open("/dev/mouse", O_RDONLY);

    /* Set mouse fd to non-blocking */
    if (mouse_fd >= 0)
        fcntl(mouse_fd, F_SETFL, O_NONBLOCK);

    /* Init compositor and cursor */
    compositor_t comp;
    comp_init(&comp, fb, backbuf, fb_w, fb_h, pitch_px);
    cursor_init(&comp.fb);
    write(2, "lumen: comp ok\n", 15);

    /* Create terminal window: 3/5 of screen, centered */
    int term_pw = fb_w * 3 / 5;
    int term_ph = fb_h * 3 / 5;
    int term_cols = term_pw / FONT_W;
    int term_rows = (term_ph - GLYPH_TITLEBAR_HEIGHT) / FONT_H;
    int master_fd = -1;
    write(2, "lumen: term skip (debug)\n", 25);
    glyph_window_t *term_win = NULL; /* skip terminal for now — fork hangs */
    (void)term_cols; (void)term_rows;
    if (term_win) {
        term_win->x = (fb_w - term_win->surf_w) / 2;
        term_win->y = (fb_h - term_win->surf_h) / 2;
        comp_add_window(&comp, term_win);
    }
    write(2, "lumen: win add\n", 15);

    glyph_window_t *info_win = create_info_window(fb_w, fb_h);
    write(2, "lumen: info win\n", 16);
    if (info_win)
        comp_add_window(&comp, info_win);

    if (term_win) {
        comp_raise_window(&comp, term_win);
        if (comp.focused)
            comp.focused->focused_window = 0;
        comp.focused = term_win;
        term_win->focused_window = 1;
    }
    write(2, "lumen: focus ok\n", 16);

    /* Do initial full composite */
    write(2, "lumen: composite\n", 17);
    comp.full_redraw = 1;
    cursor_hide();
    comp_composite(&comp);
    cursor_show(comp.cursor_x, comp.cursor_y);
    write(2, "lumen: running\n", 15);

    /* Main event loop */
    struct timespec sleep_ts = { 0, 16000000 }; /* 16ms ~ 60fps */
    char kbd_byte;
    mouse_event_t mev;
    char pty_buf[512];

    for (;;) {
        int activity = 0;
        ssize_t n;

        /* Poll keyboard (stdin, raw mode, non-blocking via VMIN=0) */
        n = read(0, &kbd_byte, 1);
        if (n == 1) {
            comp_handle_key(&comp, kbd_byte);
            activity = 1;
        }

        /* Poll mouse -- BATCH: drain all pending events */
        if (mouse_fd >= 0) {
            int16_t total_dx = 0, total_dy = 0;
            uint8_t final_buttons = 0;
            int mouse_moved = 0;
            while (1) {
                n = read(mouse_fd, &mev, sizeof(mev));
                if (n != (ssize_t)sizeof(mev))
                    break;
                total_dx += mev.dx;
                total_dy += mev.dy;
                final_buttons = mev.buttons;
                mouse_moved = 1;
            }
            if (mouse_moved) {
                comp_handle_mouse(&comp, final_buttons, total_dx, total_dy);
                activity = 1;
            }
        }

        /* Poll PTY master -- BATCH: drain all available bytes */
        if (master_fd >= 0) {
            int pty_activity = 0;
            while (1) {
                n = read(master_fd, pty_buf, sizeof(pty_buf));
                if (n <= 0)
                    break;
                terminal_write(term_win, pty_buf, (int)n);
                pty_activity = 1;
            }
            if (pty_activity)
                activity = 1;
        }

        /* Composite and cursor update. Only hide/show cursor when there's
         * actual activity — idle hide/show causes WC read flicker. */
        if (activity) {
            cursor_hide();
            comp_composite(&comp);
            cursor_show(comp.cursor_x, comp.cursor_y);
        }

        /* Sleep if idle to avoid busy-looping */
        if (!activity)
            nanosleep(&sleep_ts, NULL);
    }

    return 0;
}
