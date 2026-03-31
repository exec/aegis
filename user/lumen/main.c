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
#include <signal.h>

#include <glyph.h>
#include "font.h"
#include "cursor.h"
#include "compositor.h"
#include "dock.h"
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

static struct termios s_orig_termios;

static void
restore_terminal(void)
{
    tcsetattr(0, TCSANOW, &s_orig_termios);
}

/* ---- Clipboard ---- */

#define CLIPBOARD_MAX 8192
static char s_clipboard[CLIPBOARD_MAX];
static int s_clipboard_len = 0;

static void
clipboard_set(const char *text, int len)
{
    if (len > CLIPBOARD_MAX - 1)
        len = CLIPBOARD_MAX - 1;
    memcpy(s_clipboard, text, (unsigned)len);
    s_clipboard[len] = '\0';
    s_clipboard_len = len;
}

/* ---- Context menu ---- */

#define MENU_ITEM_ABOUT     0
#define MENU_ITEM_TERMINAL  1
#define MENU_ITEM_SEPARATOR 2
#define MENU_ITEM_POWEROFF  3
#define MENU_ITEMS          4

static const char *menu_labels[] = {
    "About Aegis", "Terminal", "---", "Power Off"
};

static int menu_open;
static int menu_hover = -1;

#define MENU_X        4
#define MENU_Y        TOPBAR_HEIGHT
#define MENU_W        160
#define MENU_ITEM_H   28
#define MENU_SEP_H    10
#define MENU_BG       0x00222238
#define MENU_HOVER_BG 0x003A3A58
#define MENU_TEXT      0x00E0E0F0
#define MENU_SEP_COL  0x00444460

static int
menu_total_height(void)
{
    int h = 8; /* top padding */
    for (int i = 0; i < MENU_ITEMS; i++) {
        if (menu_labels[i][0] == '-')
            h += MENU_SEP_H;
        else
            h += MENU_ITEM_H;
    }
    h += 8; /* bottom padding */
    return h;
}

static void
menu_draw(surface_t *fb)
{
    if (!menu_open)
        return;

    int mh = menu_total_height();

    /* Background with rounded corners */
    draw_rounded_rect(fb, MENU_X, MENU_Y, MENU_W, mh, 8, MENU_BG);

    /* Draw items */
    int y = MENU_Y + 8;
    for (int i = 0; i < MENU_ITEMS; i++) {
        if (menu_labels[i][0] == '-') {
            /* Separator line */
            draw_fill_rect(fb, MENU_X + 12, y + MENU_SEP_H / 2 - 1,
                           MENU_W - 24, 1, MENU_SEP_COL);
            y += MENU_SEP_H;
        } else {
            /* Hover highlight */
            if (i == menu_hover)
                draw_fill_rect(fb, MENU_X + 4, y, MENU_W - 8, MENU_ITEM_H,
                               MENU_HOVER_BG);

            if (g_font_ui)
                font_draw_text(fb, g_font_ui, 14, MENU_X + 16, y + 6,
                               menu_labels[i], MENU_TEXT);
            else
                draw_text(fb, MENU_X + 16, y + 4, menu_labels[i],
                          MENU_TEXT, (i == menu_hover) ? MENU_HOVER_BG : MENU_BG);
            y += MENU_ITEM_H;
        }
    }
}

/* Returns which menu item index the point is over, or -1 */
static int
menu_hit_test(int mx, int my)
{
    if (!menu_open)
        return -1;

    int mh = menu_total_height();
    if (mx < MENU_X || mx >= MENU_X + MENU_W ||
        my < MENU_Y || my >= MENU_Y + mh)
        return -1;

    int y = MENU_Y + 8;
    for (int i = 0; i < MENU_ITEMS; i++) {
        if (menu_labels[i][0] == '-') {
            y += MENU_SEP_H;
        } else {
            if (my >= y && my < y + MENU_ITEM_H)
                return i;
            y += MENU_ITEM_H;
        }
    }
    return -1;
}

/* ---- Wallpaper loading ---- */

static void
load_wallpaper(wallpaper_t *wp)
{
    wp->pixels = NULL;
    wp->w = 0;
    wp->h = 0;

    int fd = open("/usr/share/wallpaper.raw", O_RDONLY);
    if (fd < 0)
        return;

    uint32_t hdr[2];
    ssize_t n = read(fd, hdr, 8);
    if (n != 8 || hdr[0] == 0 || hdr[1] == 0 ||
        hdr[0] > 8192 || hdr[1] > 8192) {
        close(fd);
        return;
    }

    uint32_t w = hdr[0], h = hdr[1];
    size_t sz = (size_t)w * h * 4;
    uint32_t *px = malloc(sz);
    if (!px) {
        close(fd);
        return;
    }

    /* Read all pixel data (may need multiple reads) */
    size_t total = 0;
    while (total < sz) {
        n = read(fd, (char *)px + total, sz - total);
        if (n <= 0)
            break;
        total += (size_t)n;
    }
    close(fd);

    if (total < sz) {
        free(px);
        return;
    }

    wp->pixels = px;
    wp->w = w;
    wp->h = h;
}

/* ---- Terminal spawning ---- */

static compositor_t *s_comp;
static int s_fb_w, s_fb_h;

static void
spawn_terminal(compositor_t *comp, int fb_w, int fb_h)
{
    int term_pw = fb_w * 3 / 5;
    int term_ph = fb_h * 3 / 5;
    int char_w = g_font_mono ? font_text_width(g_font_mono, 16, "M") : FONT_W;
    int char_h = g_font_mono ? font_height(g_font_mono, 16) : FONT_H;
    int term_cols = term_pw / char_w;
    int term_rows = (term_ph - GLYPH_TITLEBAR_HEIGHT) / char_h;
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

/* ---- Desktop draw callback (top bar only) ---- */

static char s_clock_str[8] = "00:00";

static void
desktop_draw_cb(surface_t *s, int w, int h)
{
    (void)h;
    topbar_draw(s, w, s_clock_str);
}

/* ---- Overlay callback (frosted glass dock -- drawn after windows) ---- */

static void
overlay_draw_cb(surface_t *s, int w, int h)
{
    dock_draw(s, w, h);
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

    /* Initialize TTF font renderer (loads Inter + JetBrains Mono) */
    font_init();

    /* Ignore job control signals -- compositor always reads keyboard */
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);

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
    s_fb_w = fb_w;
    s_fb_h = fb_h;

    /* Load wallpaper (optional) */
    load_wallpaper(&comp.wallpaper);

    /* Init dock */
    dock_init(fb_w, fb_h);

    /* Desktop draw callback draws top bar; overlay draws frosted glass dock */
    comp.on_draw_desktop = desktop_draw_cb;
    comp.on_draw_overlay = overlay_draw_cb;

    /* Initial full composite — clean desktop, no windows */
    comp.full_redraw = 1;
    cursor_hide();
    comp_composite(&comp);
    cursor_show(comp.cursor_x, comp.cursor_y);

    /* Dropdown terminal -- created at startup, starts hidden */
    int dropdown_master_fd = -1;
    glyph_window_t *dropdown_win = terminal_create_dropdown(fb_w, fb_h,
                                                            &dropdown_master_fd);
    glyph_window_t *prev_focus = NULL;  /* focus before dropdown opened */

    if (dropdown_win) {
        comp_add_window(&comp, dropdown_win);
        comp_raise_window(&comp, dropdown_win);
        dropdown_win->visible = 0;
        /* Restore focus (dropdown stole it on add) */
        if (comp.nwindows > 1) {
            comp.focused = comp.windows[comp.nwindows - 2];
            comp.focused->focused_window = 1;
            dropdown_win->focused_window = 0;
        } else {
            comp.focused = NULL;
        }
        comp.full_redraw = 0;
    }

    /* Escape state for Ctrl+Alt+T detection (ESC prefix from kbd driver) */
    int esc_pending = 0;

    /* Clock update counter */
    int clock_counter = 0;

    /* Main event loop */
    struct timespec sleep_ts = { 0, 16000000 }; /* 16ms ~ 60fps */
    char kbd_byte;
    mouse_event_t mev;
    char pty_buf[512];

    for (;;) {
        int activity = 0;
        int mouse_only = 0;  /* 1 = only mouse moved, no content change */
        ssize_t n;

        /* Poll keyboard (stdin, raw mode, non-blocking via VMIN=0) */
        n = read(0, &kbd_byte, 1);
        if (n == 1) {
            /* Ctrl+C (0x03): copy selection if present, else forward to PTY */
            if (kbd_byte == 0x03) {
                if (comp.focused && comp.focused->tag > 0 &&
                    terminal_has_selection(comp.focused)) {
                    char sel_buf[CLIPBOARD_MAX];
                    int sel_len = terminal_copy_selection(comp.focused,
                                                          sel_buf, CLIPBOARD_MAX);
                    if (sel_len > 0)
                        clipboard_set(sel_buf, sel_len);
                    terminal_clear_selection(comp.focused);
                    activity = 1;
                    goto next_poll;
                }
                /* No selection — forward Ctrl+C to PTY (SIGINT via line disc) */
            }

            /* Ctrl+V (0x16): paste clipboard into focused terminal's PTY */
            if (kbd_byte == 0x16) {
                if (comp.focused && comp.focused->tag > 0 && s_clipboard_len > 0) {
                    write(comp.focused->tag, s_clipboard,
                          (unsigned)s_clipboard_len);
                    activity = 1;
                    goto next_poll;
                }
                /* No clipboard or no focused terminal — forward raw char */
            }

            /* Ctrl+Alt+T detection: kbd driver sends ESC (0x1B) as Alt
             * prefix, Ctrl masks char to 0x1F & c. Ctrl+T = 0x14.
             * So Ctrl+Alt+T = ESC then 0x14. */
            if (kbd_byte == '\033') {
                esc_pending = 1;
                activity = 1;
                goto next_poll;
            }
            if (esc_pending) {
                esc_pending = 0;
                if (kbd_byte == 0x14 && dropdown_win) {
                    /* Toggle dropdown terminal visibility */
                    if (dropdown_win->visible) {
                        /* Hide dropdown, restore previous focus */
                        dropdown_win->visible = 0;
                        dropdown_win->focused_window = 0;
                        int prev_valid = 0;
                        if (prev_focus) {
                            for (int wi = 0; wi < comp.nwindows; wi++) {
                                if (comp.windows[wi] == prev_focus) {
                                    prev_valid = 1;
                                    break;
                                }
                            }
                        }
                        if (prev_valid) {
                            comp.focused = prev_focus;
                            prev_focus->focused_window = 1;
                        } else {
                            comp.focused = NULL;
                            for (int wi = comp.nwindows - 1; wi >= 0; wi--) {
                                if (comp.windows[wi]->visible &&
                                    comp.windows[wi] != dropdown_win) {
                                    comp.focused = comp.windows[wi];
                                    comp.focused->focused_window = 1;
                                    break;
                                }
                            }
                        }
                    } else {
                        /* Show dropdown, save current focus, take focus */
                        prev_focus = comp.focused;
                        if (prev_focus)
                            prev_focus->focused_window = 0;
                        dropdown_win->visible = 1;
                        comp.focused = dropdown_win;
                        dropdown_win->focused_window = 1;
                        comp_raise_window(&comp, dropdown_win);
                        glyph_window_mark_all_dirty(dropdown_win);
                    }
                    comp.full_redraw = 1;
                    activity = 1;
                    goto next_poll;
                }

                /* Alt+C -- copy selection from focused terminal */
                if (kbd_byte == 'c' && comp.focused &&
                    terminal_has_selection(comp.focused)) {
                    char sel_buf[CLIPBOARD_MAX];
                    int sel_len = terminal_copy_selection(comp.focused,
                                                         sel_buf, CLIPBOARD_MAX);
                    if (sel_len > 0) {
                        clipboard_set(sel_buf, sel_len);
                        terminal_clear_selection(comp.focused);
                    }
                    activity = 1;
                    goto next_poll;
                }

                /* Alt+V -- paste clipboard into focused terminal's PTY */
                if (kbd_byte == 'v' && comp.focused &&
                    comp.focused->tag > 0 && s_clipboard_len > 0) {
                    write(comp.focused->tag, s_clipboard,
                          (unsigned)s_clipboard_len);
                    activity = 1;
                    goto next_poll;
                }

                /* Not a recognized Alt combo -- flush ESC + char to focused PTY */
                int mfd = (comp.focused && comp.focused->tag > 0)
                          ? comp.focused->tag : -1;
                if (mfd >= 0) {
                    char esc = '\033';
                    write(mfd, &esc, 1);
                    write(mfd, &kbd_byte, 1);
                }
                activity = 1;
                goto next_poll;
            }

            /* Normal key -- forward to focused PTY */
            {
                int mfd = (comp.focused && comp.focused->tag > 0)
                          ? comp.focused->tag : -1;
                if (mfd >= 0)
                    write(mfd, &kbd_byte, 1);
            }

            activity = 1;
        }
next_poll:

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
                /* Compute predicted cursor position for hit testing */
                int test_x = comp.cursor_x + total_dx + total_dx / 2;
                int test_y = comp.cursor_y + total_dy + total_dy / 2;
                if (test_x < 0) test_x = 0;
                if (test_y < 0) test_y = 0;
                if (test_x >= fb_w) test_x = fb_w - 1;
                if (test_y >= fb_h) test_y = fb_h - 1;

                /* Update dock hover state */
                int dock_item = dock_hit_test(test_x, test_y);
                {
                    /* Redraw dock when hover state changes */
                    static int prev_dock_hover = -1;
                    if (dock_item != prev_dock_hover) {
                        dock_set_hover(dock_item);
                        comp.full_redraw = 1;
                        prev_dock_hover = dock_item;
                    }
                }

                /* Update context menu hover */
                if (menu_open) {
                    int old_hover = menu_hover;
                    menu_hover = menu_hit_test(test_x, test_y);
                    if (menu_hover != old_hover)
                        comp.full_redraw = 1;
                }

                /* Handle button press */
                if ((final_buttons & 1) && !(comp.prev_buttons & 1)) {
                    /* Context menu click */
                    if (menu_open) {
                        int item = menu_hit_test(test_x, test_y);
                        if (item >= 0 && menu_labels[item][0] != '-') {
                            menu_open = 0;
                            menu_hover = -1;
                            comp.full_redraw = 1;
                            if (item == MENU_ITEM_TERMINAL)
                                spawn_terminal(&comp, fb_w, fb_h);
                            /* About and Power Off are stubs */
                        } else {
                            /* Click outside menu or on separator — close */
                            menu_open = 0;
                            menu_hover = -1;
                            comp.full_redraw = 1;
                        }
                        comp_handle_mouse(&comp, final_buttons, total_dx, total_dy);
                        activity = 1;
                        goto after_mouse;
                    }

                    /* Top bar "Aegis" click */
                    if (topbar_hit_aegis(test_x, test_y, fb_w)) {
                        menu_open = !menu_open;
                        menu_hover = -1;
                        comp.full_redraw = 1;
                        comp_handle_mouse(&comp, final_buttons, total_dx, total_dy);
                        activity = 1;
                        goto after_mouse;
                    }

                    /* Dock click */
                    if (dock_item >= 0) {
                        if (dock_item == DOCK_ITEM_TERMINAL)
                            spawn_terminal(&comp, fb_w, fb_h);
                        /* Settings and Files are stubs */
                        comp_handle_mouse(&comp, final_buttons, total_dx, total_dy);
                        activity = 1;
                        goto after_mouse;
                    }
                }

                comp_handle_mouse(&comp, final_buttons, total_dx, total_dy);
                /* If no buttons pressed/released and no drag, this is
                 * mouse-only movement — skip full composite */
                if (!activity && !(final_buttons & 1) && !comp.dragging)
                    mouse_only = 1;
                activity = 1;
            }
        }
after_mouse:

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

        /* Clock update — roughly once per second (60 frames) */
        clock_counter++;
        if (clock_counter >= 60) {
            clock_counter = 0;
            struct timespec ts;
            if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
                struct tm *tm = gmtime(&ts.tv_sec);
                if (tm) {
                    char new_clock[8];
                    snprintf(new_clock, sizeof(new_clock), "%02d:%02d",
                             tm->tm_hour, tm->tm_min);
                    if (strcmp(new_clock, s_clock_str) != 0) {
                        memcpy(s_clock_str, new_clock, sizeof(s_clock_str));
                        comp.full_redraw = 1;
                        activity = 1;
                    }
                }
            }
        }

        /* Composite and cursor update */
        if (activity) {
            if (mouse_only && !comp.full_redraw && !menu_open) {
                /* Mouse moved but nothing else changed — just relocate cursor.
                 * save-under handles erasing the old position on the FB.
                 * Discard any cursor-movement dirty rects so the next frame
                 * with real content changes doesn't carry stale rects. */
                comp.ndirty = 0;
                cursor_hide();
                cursor_show(comp.cursor_x, comp.cursor_y);
            } else {
                cursor_hide();
                comp_composite(&comp);
                /* Draw context menu overlay AFTER composite, on the framebuffer */
                if (menu_open)
                    menu_draw(&comp.fb);
                cursor_show(comp.cursor_x, comp.cursor_y);
            }
        }

        /* Sleep if idle to avoid busy-looping */
        if (!activity)
            nanosleep(&sleep_ts, NULL);
    }

    return 0;
}
