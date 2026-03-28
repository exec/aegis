/* main.c — Lumen compositor entry point and event loop */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <time.h>
#include <sys/syscall.h>

#include "draw.h"
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

static window_t *
create_info_window(int fb_w, int fb_h)
{
    int cols = 35;
    int rows = 10;
    int cw = cols * FONT_W;
    int ch = rows * FONT_H;
    window_t *win = calloc(1, sizeof(window_t));
    if (!win)
        return NULL;

    win->x = 20;
    win->y = 20 + TITLEBAR_HEIGHT;
    win->w = cw;
    win->h = ch;
    win->pixels = calloc(cw * ch, sizeof(uint32_t));
    if (!win->pixels) {
        free(win);
        return NULL;
    }
    strncpy(win->title, "System", sizeof(win->title) - 1);
    win->visible = 1;
    win->closeable = 1;

    /* Draw static info content */
    surface_t s = { win->pixels, cw, ch, cw };
    draw_fill_rect(&s, 0, 0, cw, ch, INFO_BG);

    int y = 8;
    int x = 10;
    int lh = FONT_H + 2;
    char buf[80];

    draw_text(&s, x, y, "SYSTEM", C_ACCENT, INFO_BG);
    y += lh + 4;

    draw_text(&s, x, y, "Kernel:   Aegis 0.1 (x86_64)", C_TEXT, INFO_BG);
    y += lh;
    draw_text(&s, x, y, "Arch:     x86_64 (AMD64)", C_TEXT, INFO_BG);
    y += lh;
    snprintf(buf, sizeof(buf), "Display:  %ux%u @ 32bpp", fb_w, fb_h);
    draw_text(&s, x, y, buf, C_TEXT, INFO_BG);
    y += lh;
    draw_text(&s, x, y, "Security: Capability-based", C_TEXT, INFO_BG);
    y += lh;
    draw_text(&s, x, y, "Shell:    oksh (OpenBSD ksh)", C_TEXT, INFO_BG);
    y += lh;
    draw_text(&s, x, y, "Init:     Vigil supervisor", C_TEXT, INFO_BG);

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
        fprintf(stderr, "[LUMEN] FAIL: sys_fb_map returned %ld\n", ret);
        return 1;
    }

    uint32_t *fb = (uint32_t *)(uintptr_t)fb_info.addr;
    int fb_w = (int)fb_info.width;
    int fb_h = (int)fb_info.height;
    int pitch_px = (int)(fb_info.pitch / (fb_info.bpp / 8));

    /* Allocate backbuffer */
    uint32_t *backbuf = malloc((size_t)pitch_px * fb_h * 4);
    if (!backbuf) {
        fprintf(stderr, "[LUMEN] FAIL: backbuffer alloc failed\n");
        return 1;
    }

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
    if (mouse_fd < 0)
        fprintf(stderr, "[LUMEN] WARN: cannot open /dev/mouse\n");

    /* Set mouse fd to non-blocking */
    if (mouse_fd >= 0)
        fcntl(mouse_fd, F_SETFL, O_NONBLOCK);

    /* Init compositor and cursor */
    compositor_t comp;
    comp_init(&comp, fb, backbuf, fb_w, fb_h, pitch_px);
    cursor_init(&comp.fb);

    fprintf(stderr, "[LUMEN] started %ux%u\n", fb_w, fb_h);

    /* Create terminal window: 3/5 of screen, centered */
    int term_pw = fb_w * 3 / 5;
    int term_ph = fb_h * 3 / 5;
    int term_cols = term_pw / FONT_W;
    int term_rows = (term_ph - TITLEBAR_HEIGHT) / FONT_H;
    int master_fd = -1;
    window_t *term_win = terminal_create(term_cols, term_rows, &master_fd);
    if (term_win) {
        /* Center the terminal */
        term_win->x = (fb_w - term_win->w) / 2;
        term_win->y = (fb_h - term_win->h) / 2;
        comp_add_window(&comp, term_win);
    }

    /* Create system info window */
    window_t *info_win = create_info_window(fb_w, fb_h);
    if (info_win)
        comp_add_window(&comp, info_win);

    /* Focus and raise terminal above info window */
    if (term_win) {
        comp_raise_window(&comp, term_win);
        comp.focused = term_win;
    }

    /* Force initial composite */
    comp.needs_redraw = 1;

    /* Main event loop */
    struct timespec sleep_ts = { 0, 16000000 }; /* 16ms ~ 60fps */
    char kbd_byte;
    mouse_event_t mev;
    char pty_buf[512];

    for (;;) {
        int activity = 0;

        /* Poll keyboard (stdin, raw mode, non-blocking via VMIN=0) */
        ssize_t n = read(0, &kbd_byte, 1);
        if (n == 1) {
            comp_handle_key(&comp, kbd_byte);
            activity = 1;
        }

        /* Poll mouse */
        if (mouse_fd >= 0) {
            n = read(mouse_fd, &mev, sizeof(mev));
            if (n == (ssize_t)sizeof(mev)) {
                comp_handle_mouse(&comp, mev.buttons, mev.dx, mev.dy);
                activity = 1;
            }
        }

        /* Poll PTY master */
        if (master_fd >= 0) {
            n = read(master_fd, pty_buf, sizeof(pty_buf));
            if (n > 0) {
                terminal_write(term_win, pty_buf, (int)n);
                comp.needs_redraw = 1;
                activity = 1;
            }
        }

        /* Composite if dirty */
        if (comp.needs_redraw) {
            cursor_hide();
            comp_composite(&comp);
            cursor_show(comp.cursor_x, comp.cursor_y);
            comp.needs_redraw = 0;
        }

        /* Sleep if idle to avoid busy-looping */
        if (!activity)
            nanosleep(&sleep_ts, NULL);
    }

    return 0;
}
