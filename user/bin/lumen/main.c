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
#include <poll.h>
#include <signal.h>
#include <glyph.h>
#include <font.h>
#include "cursor.h"
#include "compositor.h"
#include <taskbar.h>
#include <theme.h>
#include "terminal.h"
#include "widget_test.h"
#include "about.h"
#include "lumen_server.h"

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
static volatile sig_atomic_t s_input_frozen;

static void sigusr2_handler(int sig) { (void)sig; s_input_frozen = 0; }

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
#define MENU_ITEM_SETTINGS  1
#define MENU_ITEM_LOCK      2
#define MENU_ITEM_SEPARATOR 3
#define MENU_ITEM_REBOOT    4
#define MENU_ITEM_POWEROFF  5
#define MENU_ITEMS          6

static const char *menu_labels[] = {
    "About Aegis", "Settings...", "Lock Screen", "---", "Restart", "Power Off"
};

#define SYS_REBOOT 169

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
menu_draw(surface_t *s)
{
    if (!menu_open)
        return;

    int mh = menu_total_height();

    /* Frosted glass background: blur + tint + rounded corner mask */
    draw_box_blur(s, MENU_X, MENU_Y, MENU_W, mh, 10);
    draw_blend_rect(s, MENU_X, MENU_Y, MENU_W, mh, MENU_BG, 180);

    /* Mask corners by painting desktop bg over outside-rounded pixels.
     * Only check the corner regions (r=8). */
    {
        int r = 8, mx = MENU_X, my = MENU_Y, mw = MENU_W;
        for (int py = 0; py < r; py++) {
            for (int px = 0; px < r; px++) {
                int dx = r - px, dy = r - py;
                if (dx * dx + dy * dy > r * r) {
                    /* Top-left */
                    draw_px(s, mx + px, my + py, C_BG1);
                    /* Top-right */
                    draw_px(s, mx + mw - 1 - px, my + py, C_BG1);
                    /* Bottom-left */
                    draw_px(s, mx + px, my + mh - 1 - py, C_BG1);
                    /* Bottom-right */
                    draw_px(s, mx + mw - 1 - px, my + mh - 1 - py, C_BG1);
                }
            }
        }
    }

    /* Subtle border */
    draw_blend_rect(s, MENU_X, MENU_Y, MENU_W, 1, 0x00FFFFFF, 20);
    draw_blend_rect(s, MENU_X, MENU_Y + mh - 1, MENU_W, 1, 0x00000000, 30);
    draw_blend_rect(s, MENU_X, MENU_Y, 1, mh, 0x00FFFFFF, 15);
    draw_blend_rect(s, MENU_X + MENU_W - 1, MENU_Y, 1, mh, 0x00000000, 20);

    /* Draw items */
    int y = MENU_Y + 8;
    for (int i = 0; i < MENU_ITEMS; i++) {
        if (menu_labels[i][0] == '-') {
            draw_blend_rect(s, MENU_X + 12, y + MENU_SEP_H / 2 - 1,
                            MENU_W - 24, 1, MENU_SEP_COL, 120);
            y += MENU_SEP_H;
        } else {
            if (i == menu_hover)
                draw_blend_rect(s, MENU_X + 4, y, MENU_W - 8, MENU_ITEM_H,
                                MENU_HOVER_BG, 100);

            if (g_font_ui)
                font_draw_text(s, g_font_ui, 14, MENU_X + 16, y + 6,
                               menu_labels[i], MENU_TEXT);
            else
                draw_text(s, MENU_X + 16, y + 4, menu_labels[i],
                          MENU_TEXT, (i == menu_hover) ? MENU_HOVER_BG : MENU_BG);
            y += MENU_ITEM_H;
        }
    }
}

static glyph_rect_t
menu_rect(void)
{
    return (glyph_rect_t){MENU_X, MENU_Y, MENU_W, menu_total_height()};
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

    dprintf(2, "[LUMEN-TERM] spawned, win=%p master_fd=%d\n",
            (void *)term_win, master_fd);

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
    dprintf(2, "[LUMEN-TERM] focus set: focused=%p tag=%d\n",
            (void *)comp->focused, comp->focused->tag);
}

/* ---- Desktop draw callback (top bar only) ---- */

static char s_clock_str[8] = "00:00";

static void
desktop_draw_cb(surface_t *s, int w, int h)
{
    (void)h;
    topbar_draw(s, w, s_clock_str);
}

/* ---- Overlay callback -- drawn after windows (context menu only) ---- */

static void
overlay_draw_cb(surface_t *s, int w, int h)
{
    (void)w; (void)h;
    menu_draw(s);
}

/* ---- Built-in spawners exposed to citadel-dock via LUMEN_OP_INVOKE ---- */

static void
spawn_widgets(compositor_t *comp, int fb_w, int fb_h)
{
    glyph_window_t *wt = widget_test_create();
    if (!wt) return;
    wt->x = (fb_w - wt->surf_w) / 2;
    wt->y = (fb_h - wt->surf_h) / 2;
    comp_add_window(comp, wt);
    comp_raise_window(comp, wt);
    comp->focused = wt;
    wt->focused_window = 1;
    glyph_window_mark_all_dirty(wt);
}

/* Spawn an external Lumen client by absolute path. The child connects
 * back to /run/lumen.sock via the external window protocol. fd 0/1/2
 * are inherited from the compositor's stderr (which is /dev/console),
 * so the child's diag prints reach the serial harness. */
#define SYS_SPAWN 514
static long
spawn_external_client(const char *path)
{
    char *argv[] = { (char *)path, NULL };
    char *envp[] = { "PATH=/bin", "HOME=/root", "USER=root", NULL };
    return syscall(SYS_SPAWN, path, argv, envp, 2 /* stderr→/dev/console */, 0);
}

static void
invoke_handler(compositor_t *comp, const char *name)
{
    if (!name) return;
    if (strcmp(name, "terminal") == 0) {
        spawn_terminal(comp, s_fb_w, s_fb_h);
        dprintf(2, "[LUMEN] window_opened=terminal\n");
    } else if (strcmp(name, "widgets") == 0) {
        spawn_widgets(comp, s_fb_w, s_fb_h);
        dprintf(2, "[LUMEN] window_opened=widgets\n");
    } else if (strcmp(name, "gui-installer") == 0) {
        long pid = spawn_external_client("/bin/gui-installer");
        dprintf(2, "[LUMEN] window_opened=gui-installer pid=%ld\n", pid);
    } else {
        dprintf(2, "[LUMEN] invoke: unknown name=%s\n", name);
    }
}

int
main(void)
{
    /* Refuse to run nested. Lumen sets LUMEN_RUNNING=1 below before
     * spawning any children, so any descendant that re-execs lumen
     * (e.g. user types `lumen` in a Lumen terminal) will trip this
     * guard. Without it the framebuffer gets mapped twice, two cursors
     * appear, ctrl+c gets swallowed, and various other small horrors
     * unfold gracefully. */
    if (getenv("LUMEN_RUNNING")) {
        const char *msg = "lumen: you're already using lumen, pal\n";
        write(2, msg, strlen(msg));
        return 1;
    }
    setenv("LUMEN_RUNNING", "1", 1);

    /* Note: Caps come from kernel policy at exec time. Bastion (parent)
     * handles cap delegation. Lumen runs with baseline caps only. */

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
    signal(SIGUSR2, sigusr2_handler);

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

    /* Start external window server */
    int lumen_srv_fd = lumen_server_init();
    if (lumen_srv_fd < 0)
        dprintf(2, "[LUMEN] warning: could not open /run/lumen.sock\n");

    /* Load wallpaper (optional) */
    load_wallpaper(&comp.wallpaper);

    /* Register INVOKE handler so external dock can spawn built-ins. */
    lumen_server_set_invoke_handler(invoke_handler);

    /* Desktop draw callback draws top bar; overlay draws context menu */
    comp.on_draw_desktop = desktop_draw_cb;
    comp.on_draw_overlay = overlay_draw_cb;

    /* Snapshot current FB (Bastion's login form) BEFORE first composite */
    size_t fb_bytes = (size_t)pitch_px * fb_h * 4;
    size_t npx = (size_t)pitch_px * fb_h;
    uint32_t *saved_frame = malloc(fb_bytes);
    if (saved_frame)
        memcpy(saved_frame, fb, fb_bytes);

    /* Initial full composite — renders desktop to backbuf, then copies to FB.
     * We need the backbuf result but NOT the FB write. Temporarily swap FB
     * pointer so comp_composite writes to a throwaway buffer. */
    comp.full_redraw = 1;
    cursor_hide();

    /* Save real FB pointer, point compositor at backbuf (it'll memcpy backbuf→"fb"
     * which is a no-op since they're the same). Then restore. */
    uint32_t *real_fb = comp.fb.buf;
    comp.fb.buf = backbuf;  /* composite writes backbuf→backbuf (harmless) */
    comp_composite(&comp);
    comp.fb.buf = real_fb;  /* restore real FB */

    /* Crossfade from saved Bastion frame → composited desktop (in backbuf) */
    if (saved_frame) {
        struct timespec ts = { 0, 17000000 }; /* 17ms per step — 250ms total */
        for (int step = 0; step < 15; step++) {
            int alpha = 255 - (step * 255 / 14);
            int inv = 255 - alpha;
            for (size_t i = 0; i < npx; i++) {
                uint32_t old = saved_frame[i];
                uint32_t new_px = backbuf[i];
                uint32_t r = (((old >> 16) & 0xFF) * alpha + ((new_px >> 16) & 0xFF) * inv) / 255;
                uint32_t g = (((old >> 8) & 0xFF) * alpha + ((new_px >> 8) & 0xFF) * inv) / 255;
                uint32_t b = ((old & 0xFF) * alpha + (new_px & 0xFF) * inv) / 255;
                fb[i] = (r << 16) | (g << 8) | b;
            }
            nanosleep(&ts, NULL);
        }
        free(saved_frame);
    }
    /* Final: copy actual desktop to FB cleanly */
    memcpy(fb, backbuf, fb_bytes);

    /* Signal test harness: fade-in complete, desktop is on screen */
    dprintf(2, "[LUMEN] ready\n");

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

    (void)0; /* esc_pending removed — sequences collected inline */

    /* First-boot disclaimer: show the About window (which carries the v1
     * software disclaimer) on every Lumen startup.  Users can close it
     * immediately; it's a reminder, not a blocker. */
    {
        glyph_window_t *aw = about_create(fb_w, fb_h);
        if (aw) {
            comp_add_window(&comp, aw);
            comp_raise_window(&comp, aw);
            comp.focused = aw;
            aw->focused_window = 1;
            glyph_window_mark_all_dirty(aw);
            comp.full_redraw = 1;
        }
    }

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

        /* Service external window clients */
        if (lumen_srv_fd >= 0) {
            if (lumen_server_tick(&comp, lumen_srv_fd))
                activity = 1;
        }

        /* Poll keyboard (stdin, raw mode, non-blocking via VMIN=0) */
        n = read(0, &kbd_byte, 1);
        if (n == 1 && s_input_frozen) {
            /* Screen locked — discard all input */
            goto next_poll;
        }
        if (n == 1) {
            /* ESC prefix handling: collect the full escape sequence
             * immediately (no sleeping between bytes).  The kbd ISR pushes
             * all bytes of an escape sequence in one handler invocation,
             * so they are in the ring buffer within microseconds. */
            if (kbd_byte == '\033') {
                /* Try to read the next byte after ESC.  The kbd ISR
                 * pushes all bytes of an escape sequence together, but
                 * for USB HID the bytes come from PIT ISR polling at
                 * 100 Hz — the next byte might not arrive for up to
                 * 10 ms.  Use poll() with a short timeout. */
                char eb = 0;
                int got_eb = 0;
                {
                    struct pollfd pfd = { .fd = 0, .events = POLLIN };
                    if (poll(&pfd, 1, 15) > 0)  /* 15ms — covers one PIT tick */
                        got_eb = (read(0, &eb, 1) == 1);
                }

                if (!got_eb) {
                    /* Bare ESC — dispatch to focused window's on_key */
                    if (comp.focused && comp.focused->on_key) {
                        comp.focused->on_key(comp.focused, '\033');
                    } else {
                        int mfd = (comp.focused && comp.focused->tag >= 0)
                                  ? comp.focused->tag : -1;
                        if (mfd >= 0) {
                            char esc = '\033';
                            write(mfd, &esc, 1);
                        }
                    }
                    activity = 1;
                    goto next_poll;
                }

                /* Ctrl+Alt+T (0x14) — toggle dropdown */
                if (eb == 0x14 && dropdown_win) {
                    if (dropdown_win->visible) {
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

                /* Ctrl+Alt+I (0x09) — launch GUI installer */
                if (eb == 0x09) {
                    invoke_handler(&comp, "gui-installer");
                    activity = 1;
                    goto next_poll;
                }

                /* Ctrl+Alt+L (0x0C) — lock screen */
                if (eb == 0x0C) {
                    s_input_frozen = 1;
                    kill(getppid(), SIGUSR1);
                    activity = 1;
                    goto next_poll;
                }

                /* Ctrl+Shift+C (0x03) or Alt+C ('c') — copy */
                if ((eb == 0x03 || eb == 'c') && comp.focused &&
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

                /* Ctrl+Shift+V (0x16) or Alt+V ('v') — paste */
                if ((eb == 0x16 || eb == 'v') && comp.focused &&
                    comp.focused->tag >= 0 && s_clipboard_len > 0) {
                    write(comp.focused->tag, s_clipboard,
                          (unsigned)s_clipboard_len);
                    activity = 1;
                    goto next_poll;
                }

                /* CSI sequence (ESC [): collect params + final byte,
                 * then write the whole sequence in one shot. */
                if (eb == '[') {
                    /* Collect the CSI sequence regardless of target so we
                     * can either pass it through to a PTY or translate the
                     * common arrow keys into single-byte synthetic codes
                     * for proxy windows (which only see one byte at a time
                     * via on_key). Synthetic codes use the high range
                     * (0xF0-0xFF) so they don't collide with ASCII or
                     * UTF-8 bytes. gui-installer maps LEFT→back,
                     * RIGHT→next; new clients can do the same. */
                    char seq[16] = { '\033', '[' };
                    int slen = 2;
                    char cb;
                    while (slen < 15) {
                        struct pollfd cpfd = { .fd = 0, .events = POLLIN };
                        int got = 0;
                        if (poll(&cpfd, 1, 15) > 0)
                            got = (read(0, &cb, 1) == 1);
                        if (!got) break;
                        seq[slen++] = cb;
                        if (cb >= 0x40 && cb <= 0x7E)
                            break;  /* final byte */
                    }

                    int mfd = (comp.focused && comp.focused->tag >= 0)
                              ? comp.focused->tag : -1;
                    if (mfd >= 0) {
                        write(mfd, seq, (unsigned)slen);
                    } else if (comp.focused && comp.focused->on_key &&
                               slen == 3) {
                        char synth = 0;
                        switch (seq[2]) {
                        case 'A': synth = (char)0xF1; break; /* Up */
                        case 'B': synth = (char)0xF2; break; /* Down */
                        case 'C': synth = (char)0xF3; break; /* Right */
                        case 'D': synth = (char)0xF4; break; /* Left */
                        }
                        if (synth) comp.focused->on_key(comp.focused, synth);
                    }
                    activity = 1;
                    goto next_poll;
                }

                /* Alt+key or unrecognized ESC combo — forward ESC + byte
                 * to focused PTY as a single write so crossterm sees it
                 * atomically. */
                {
                    int mfd = (comp.focused && comp.focused->tag >= 0)
                              ? comp.focused->tag : -1;
                    if (mfd >= 0) {
                        char pair[2] = { '\033', eb };
                        write(mfd, pair, 2);
                    }
                }
                activity = 1;
                goto next_poll;
            }

            /* Normal key -- prefer the window's on_key callback so external
             * proxy windows (gui-installer etc.) get LUMEN_EV_KEY. Fall back
             * to a direct PTY write only when the focused window has no
             * on_key handler (legacy path). */
            if (comp.focused && comp.focused->on_key) {
                comp.focused->on_key(comp.focused, kbd_byte);
            } else {
                int mfd = (comp.focused && comp.focused->tag >= 0)
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

                /* Update context menu hover */
                if (menu_open) {
                    int old_hover = menu_hover;
                    menu_hover = menu_hit_test(test_x, test_y);
                    if (menu_hover != old_hover)
                        comp_add_dirty(&comp, menu_rect());
                }

                /* Handle button press */
                if ((final_buttons & 1) && !(comp.prev_buttons & 1)) {
                    /* Context menu click */
                    if (menu_open) {
                        int item = menu_hit_test(test_x, test_y);
                        if (item >= 0 && menu_labels[item][0] != '-') {
                            comp_add_dirty(&comp, menu_rect());
                            menu_open = 0;
                            menu_hover = -1;
                            if (item == MENU_ITEM_ABOUT) {
                                glyph_window_t *aw = about_create(fb_w, fb_h);
                                if (aw) {
                                    comp_add_window(&comp, aw);
                                    comp_raise_window(&comp, aw);
                                    comp.focused = aw;
                                    aw->focused_window = 1;
                                    glyph_window_mark_all_dirty(aw);
                                }
                            } else if (item == MENU_ITEM_LOCK) {
                                s_input_frozen = 1;
                                kill(getppid(), SIGUSR1);
                            } else if (item == MENU_ITEM_REBOOT) {
                                syscall(SYS_REBOOT, 1L);  /* reboot */
                            } else if (item == MENU_ITEM_POWEROFF) {
                                /* Signal init (PID 1) for graceful shutdown.
                                 * Same path as physical ACPI power button. */
                                kill(1, SIGTERM);
                            }
                            /* Settings is a stub for now */
                        } else {
                            /* Click outside menu or on separator — close */
                            comp_add_dirty(&comp, menu_rect());
                            menu_open = 0;
                            menu_hover = -1;
                        }
                        comp_handle_mouse(&comp, final_buttons, total_dx, total_dy);
                        activity = 1;
                        goto after_mouse;
                    }

                    /* Top bar "Aegis" click */
                    if (topbar_hit_aegis(test_x, test_y, fb_w)) {
                        comp_add_dirty(&comp, menu_rect());
                        menu_open = !menu_open;
                        menu_hover = -1;
                        comp_handle_mouse(&comp, final_buttons, total_dx, total_dy);
                        activity = 1;
                        goto after_mouse;
                    }

                    /* Dock clicks are now handled by /bin/citadel-dock
                     * via the LUMEN_OP_INVOKE protocol opcode. */
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
            if (win->tag < 0)
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
                        /* Dirty just the topbar region */
                        comp_add_dirty(&comp, (glyph_rect_t){0, 0, comp.fb.w, 28});
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
                cursor_show(comp.cursor_x, comp.cursor_y);
            }
        }

        /* Second PTY read pass — after compositing, the shell has had
         * time to run (sched_tick during composite's FB writes).  This
         * catches output that wasn't ready during the first pass. */
        if (activity) {
            for (int wi = 0; wi < comp.nwindows; wi++) {
                glyph_window_t *win = comp.windows[wi];
                if (win->tag < 0) continue;
                int mfd = win->tag;
                int late = 0;
                while ((n = read(mfd, pty_buf, sizeof(pty_buf))) > 0) {
                    terminal_write(win, pty_buf, (int)n);
                    late = 1;
                }
                if (late) {
                    cursor_hide();
                    comp_composite(&comp);
                    cursor_show(comp.cursor_x, comp.cursor_y);
                }
            }
        }

        /* Sleep if idle to avoid busy-looping */
        if (!activity)
            nanosleep(&sleep_ts, NULL);
    }

    return 0;
}
