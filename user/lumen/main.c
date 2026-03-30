/* main.c -- Lumen compositor entry point and event loop */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <time.h>
#include <sys/syscall.h>
#include <errno.h>

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
    draw_text(s, x, y, "Shell:    /bin/sh", C_TEXT, INFO_BG);
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

/* ---- Desktop icons ---- */

#define ICON_SIZE   48
#define ICON_LABEL_H (FONT_H + 4)
#define MAX_ICONS   8

typedef struct {
    int x, y;
    const char *label;
    int id;
} desktop_icon_t;

static desktop_icon_t s_icons[MAX_ICONS];
static int s_nicons;

static void
desktop_add_icon(int x, int y, const char *label, int id)
{
    if (s_nicons >= MAX_ICONS)
        return;
    s_icons[s_nicons].x = x;
    s_icons[s_nicons].y = y;
    s_icons[s_nicons].label = label;
    s_icons[s_nicons].id = id;
    s_nicons++;
}

static void desktop_draw_icons(surface_t *s);

/* Callback for compositor — draws icons on desktop background */
static void
desktop_draw_icons_cb(surface_t *s, int w, int h)
{
    (void)w; (void)h;
    desktop_draw_icons(s);
}

static void
desktop_draw_icons(surface_t *s)
{
    for (int i = 0; i < s_nicons; i++) {
        desktop_icon_t *ic = &s_icons[i];

        /* Icon background (rounded-ish rect) */
        draw_fill_rect(s, ic->x, ic->y, ICON_SIZE, ICON_SIZE, 0x00334455);
        draw_rect(s, ic->x, ic->y, ICON_SIZE, ICON_SIZE, C_ACCENT);

        /* Simple terminal icon: ">_" text centered */
        if (ic->id == 1) {
            draw_text(s, ic->x + 9, ic->y + 14, ">_", 0x0000FF88, 0x00334455);
        } else if (ic->id == 2) {
            draw_text(s, ic->x + 14, ic->y + 14, "i", C_ACCENT, 0x00334455);
        }

        /* Label below icon */
        int lx = ic->x + (ICON_SIZE - (int)strlen(ic->label) * FONT_W) / 2;
        draw_text_t(s, lx, ic->y + ICON_SIZE + 4, ic->label, 0x00CCDDEE);
    }
}

static int
desktop_hit_icon(int mx, int my)
{
    for (int i = 0; i < s_nicons; i++) {
        desktop_icon_t *ic = &s_icons[i];
        if (mx >= ic->x && mx < ic->x + ICON_SIZE &&
            my >= ic->y && my < ic->y + ICON_SIZE + ICON_LABEL_H)
            return ic->id;
    }
    return 0;
}

/* ---- Terminal spawning ---- */

static compositor_t *s_comp;  /* for desktop icon handler */

static void
spawn_terminal(compositor_t *comp, int fb_w, int fb_h)
{
    int term_pw = fb_w * 3 / 5;
    int term_ph = fb_h * 3 / 5;
    int term_cols = term_pw / FONT_W;
    int term_rows = (term_ph - GLYPH_TITLEBAR_HEIGHT) / FONT_H;
    int master_fd = -1;

    glyph_window_t *term_win = terminal_create(term_cols, term_rows, &master_fd);
    if (!term_win)
        return;

    term_win->x = (fb_w - term_win->surf_w) / 2;
    term_win->y = (fb_h - term_win->surf_h) / 2;
    comp_add_window(comp, term_win);
    comp_raise_window(comp, term_win);

    /* Focus the new terminal */
    if (comp->focused && comp->focused != term_win)
        comp->focused->focused_window = 0;
    comp->focused = term_win;
    term_win->focused_window = 1;

    /* Store master_fd in the window's tag for polling */
    term_win->tag = master_fd;
}

int
main(void)
{
    fb_info_t fb_info;
    memset(&fb_info, 0, sizeof(fb_info));

    /* Map framebuffer via sys_fb_map (513) */
    long ret = syscall(513, &fb_info);
    if (ret < 0)
        return 1;

    uint32_t *fb = (uint32_t *)(uintptr_t)fb_info.addr;
    int fb_w = (int)fb_info.width;
    int fb_h = (int)fb_info.height;
    int pitch_px = (int)(fb_info.pitch / (fb_info.bpp / 8));

    /* Allocate backbuffer */
    uint32_t *backbuf = malloc((size_t)pitch_px * fb_h * 4);
    if (!backbuf)
        return 1;

    /* Set stdin to raw mode */
    tcgetattr(0, &s_orig_termios);
    atexit(restore_terminal);
    struct termios raw = s_orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(0, TCSANOW, &raw);

    /* Open mouse device */
    int mouse_fd = open("/dev/mouse", O_RDONLY);
    if (mouse_fd >= 0)
        fcntl(mouse_fd, F_SETFL, O_NONBLOCK);

    /* Init compositor and cursor */
    compositor_t comp;
    comp_init(&comp, fb, backbuf, fb_w, fb_h, pitch_px);
    cursor_init(&comp.fb);
    s_comp = &comp;

    /* Desktop icons */
    desktop_add_icon(fb_w - 80, 20, "Terminal", 1);
    desktop_add_icon(fb_w - 80, 110, "System", 2);
    comp.on_draw_desktop = desktop_draw_icons_cb;

    /* Create windows */
    glyph_window_t *info_win = create_info_window(fb_w, fb_h);
    if (info_win)
        comp_add_window(&comp, info_win);

    /* Spawn initial terminal — uses sys_spawn, no fork, instant */
    spawn_terminal(&comp, fb_w, fb_h);

    /* Initial full composite (icons drawn via on_draw_desktop callback) */
    comp.full_redraw = 1;
    cursor_hide();
    comp_composite(&comp);
    cursor_show(comp.cursor_x, comp.cursor_y);

    /* Debug status bar — draws directly to FB, top-left */
    int dbg_key_count = 0;
    char dbg_line[80];

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
        dbg_key_count++;
        if (n == 1) {
            int mfd = (comp.focused && comp.focused->tag > 0)
                      ? comp.focused->tag : -1;
            int wr = -99;
            if (mfd >= 0)
                wr = (int)write(mfd, &kbd_byte, 1);
            snprintf(dbg_line, sizeof(dbg_line),
                     "KEY 0x%02x '%c'  focus=%s mfd=%d wr=%d   ",
                     (unsigned char)kbd_byte,
                     (kbd_byte >= 0x20 && kbd_byte < 0x7f) ? kbd_byte : '.',
                     comp.focused ? "yes" : "no",
                     mfd, wr);
            /* Key already written to PTY above; skip comp_handle_key
             * to avoid double-write.  TODO: remove debug overlay. */
            activity = 1;
        } else {
            /* Show read result every ~60 frames to avoid flicker */
            if ((dbg_key_count % 60) == 0) {
                extern int errno;
                snprintf(dbg_line, sizeof(dbg_line),
                         "read(0)=%d errno=%d  poll#%d  focus=%s nwin=%d   ",
                         (int)n, errno, dbg_key_count,
                         comp.focused ? "yes" : "no",
                         comp.nwindows);
                activity = 1;  /* force redraw to show debug */
            }
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
                /* Check for desktop icon click (button press on background) */
                if ((final_buttons & 1) && !(comp.prev_buttons & 1)) {
                    int test_x = comp.cursor_x + total_dx;
                    int test_y = comp.cursor_y + total_dy;
                    if (test_x < 0) test_x = 0;
                    if (test_y < 0) test_y = 0;
                    if (!comp_window_at(&comp, test_x, test_y)) {
                        int icon_id = desktop_hit_icon(test_x, test_y);
                        if (icon_id == 1)
                            spawn_terminal(&comp, fb_w, fb_h);
                    }
                }
                comp_handle_mouse(&comp, final_buttons, total_dx, total_dy);
                activity = 1;
            }
        }

        /* Poll PTY masters for ALL open terminal windows */
        for (int wi = 0; wi < comp.nwindows; wi++) {
            glyph_window_t *win = comp.windows[wi];
            if (win->tag <= 0)
                continue;
            int mfd = win->tag;
            int pty_activity = 0;
            while (1) {
                n = read(mfd, pty_buf, sizeof(pty_buf));
                if (n <= 0)
                    break;
                terminal_write(win, pty_buf, (int)n);
                pty_activity = 1;
            }
            if (pty_activity)
                activity = 1;
        }

        /* Composite and cursor update */
        if (activity) {
            cursor_hide();
            comp_composite(&comp);
            /* Debug overlay — draw on FB AFTER composite so it persists */
            {
                surface_t dbg_surf = { fb, fb_w, fb_h, pitch_px };
                draw_fill_rect(&dbg_surf, 0, 0, fb_w, FONT_H + 2, 0x00200000);
                draw_text(&dbg_surf, 4, 1, dbg_line, 0x0000FF00, 0x00200000);
            }
            cursor_show(comp.cursor_x, comp.cursor_y);
        }

        /* Sleep if idle to avoid busy-looping */
        if (!activity)
            nanosleep(&sleep_ts, NULL);
    }

    return 0;
}
