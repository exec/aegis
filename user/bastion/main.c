/* bastion — graphical display manager for Aegis.
 *
 * Presents a login form, authenticates via libauth, then spawns Lumen
 * as the authenticated user's session. Handles lock/unlock via SIGUSR1/2.
 *
 * Vigil service: graphical mode, respawn policy.
 * Caps from capd: AUTH, CAP_GRANT, CAP_DELEGATE, CAP_QUERY, SETUID.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <termios.h>
#include <stdint.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/syscall.h>

#include <glyph.h>
#include "auth.h"

extern char **environ;

#define SYS_FB_MAP  513
#define SYS_SPAWN   514

/* ---- Framebuffer ----------------------------------------------------- */

typedef struct {
    uint64_t addr;
    uint32_t width, height, pitch, bpp;
} fb_info_t;

/* ---- Mouse event (matches /dev/mouse) -------------------------------- */

typedef struct __attribute__((packed)) {
    uint8_t  buttons;
    int16_t  dx;
    int16_t  dy;
    int16_t  scroll;
} mouse_event_t;

/* ---- State ----------------------------------------------------------- */

static fb_info_t  s_fb_info;
static uint32_t  *s_fb;
static uint32_t  *s_backbuf;
static int        s_fb_w, s_fb_h, s_pitch_px;

static volatile sig_atomic_t s_locked;
static pid_t      s_lumen_pid;
static char       s_username[64]; /* remembered for lock screen */
static int        s_uid, s_gid;

/* ---- Logo ------------------------------------------------------------ */

static uint32_t *s_logo_pixels;
static int       s_logo_w, s_logo_h;

static void
load_logo(void)
{
    /* Try to load raw BGRA logo: first 8 bytes are uint32_t w, h */
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
    s_logo_pixels = malloc(sz);
    if (!s_logo_pixels) { close(fd); s_logo_w = s_logo_h = 0; return; }
    size_t got = 0;
    while (got < sz) {
        ssize_t n = read(fd, (char *)s_logo_pixels + got, sz - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(fd);
    if (got < sz) { free(s_logo_pixels); s_logo_pixels = NULL; s_logo_w = s_logo_h = 0; }
}

/* ---- Drawing helpers ------------------------------------------------- */

static void
fill_bg(void)
{
    for (int y = 0; y < s_fb_h; y++)
        for (int x = 0; x < s_fb_w; x++)
            s_backbuf[y * s_pitch_px + x] = 0xFF1A1A2E;
}

static void
blit_to_fb(void)
{
    for (int y = 0; y < s_fb_h; y++)
        memcpy(s_fb + y * s_pitch_px, s_backbuf + y * s_pitch_px,
               (size_t)s_fb_w * 4);
}

static void
draw_logo(int cx, int y)
{
    if (!s_logo_pixels) return;
    /* Draw at 50% scale */
    int dw = s_logo_w / 2;
    int dh = s_logo_h / 2;
    int x0 = cx - dw / 2;
    for (int dy_off = 0; dy_off < dh; dy_off++) {
        int dy = y + dy_off;
        if (dy < 0 || dy >= s_fb_h) continue;
        int ly = dy_off * 2;  /* nearest-neighbor sample */
        for (int dx_off = 0; dx_off < dw; dx_off++) {
            int dx = x0 + dx_off;
            if (dx < 0 || dx >= s_fb_w) continue;
            int lx = dx_off * 2;
            uint32_t px = s_logo_pixels[ly * s_logo_w + lx];
            uint32_t a = (px >> 24) & 0xFF;
            if (a == 0xFF) {
                s_backbuf[dy * s_pitch_px + dx] = px;
            } else if (a > 0) {
                uint32_t bg = s_backbuf[dy * s_pitch_px + dx];
                uint32_t rb = (((px & 0xFF00FF) * a + (bg & 0xFF00FF) * (255-a)) >> 8) & 0xFF00FF;
                uint32_t g  = (((px & 0x00FF00) * a + (bg & 0x00FF00) * (255-a)) >> 8) & 0x00FF00;
                s_backbuf[dy * s_pitch_px + dx] = 0xFF000000 | rb | g;
            }
        }
    }
}

static void
draw_text_simple(int x, int y, const char *text, uint32_t color)
{
    /* Minimal 8x16 text — use Glyph's bitmap font via surface */
    surface_t s = { .buf = s_backbuf, .w = s_fb_w, .h = s_fb_h, .pitch = s_pitch_px };
    draw_text_t(&s, x, y, text, color);
}

/* ---- Crossfade helper ------------------------------------------------ */

static uint32_t *s_saved_frame;  /* snapshot of FB before first draw */

static void
crossfade(int steps, int delay_ms)
{
    if (!s_saved_frame) return;
    size_t npx = (size_t)s_pitch_px * s_fb_h;
    struct timespec ts = { 0, delay_ms * 1000000L };

    for (int step = 0; step < steps; step++) {
        int alpha = 255 - (step * 255 / (steps - 1));  /* 255 → 0 */
        int inv = 255 - alpha;
        for (size_t i = 0; i < npx; i++) {
            uint32_t old = s_saved_frame[i];
            uint32_t new_px = s_backbuf[i];
            uint32_t r = (((old >> 16) & 0xFF) * alpha + ((new_px >> 16) & 0xFF) * inv) / 255;
            uint32_t g = (((old >> 8) & 0xFF) * alpha + ((new_px >> 8) & 0xFF) * inv) / 255;
            uint32_t b = ((old & 0xFF) * alpha + (new_px & 0xFF) * inv) / 255;
            s_fb[i] = (r << 16) | (g << 8) | b;
        }
        nanosleep(&ts, NULL);
    }
    free(s_saved_frame);
    s_saved_frame = NULL;
}

/* ---- Login form ------------------------------------------------------ */

#define FIELD_W     240
#define FIELD_H     32
#define FIELD_GAP   10
#define BTN_H       34

static char s_user_buf[64];
static char s_pass_buf[128];
static int  s_user_len, s_pass_len;
static int  s_focus; /* 0=username, 1=password, 2=button */
static char s_error[128];
static int  s_is_lock; /* 1 = lock screen mode */

static void
draw_form(void)
{
    /* Charcoal background — matches GRUB splash feel */
    for (int y = 0; y < s_fb_h; y++)
        for (int x = 0; x < s_fb_w; x++)
            s_backbuf[y * s_pitch_px + x] = 0x00202030;

    int cx = s_fb_w / 2;
    surface_t surf = { .buf = s_backbuf, .w = s_fb_w, .h = s_fb_h, .pitch = s_pitch_px };

    /* Centered logo — match GRUB splash position (dead center vertically) */
    int logo_dh = s_logo_h > 0 ? s_logo_h / 4 : 20;
    int logo_y = s_fb_h / 2 - logo_dh / 2;
    if (s_logo_pixels) {
        draw_logo(cx, logo_y);
    } else {
        draw_text_simple(cx - 5 * 8 / 2, logo_y, "AEGIS", 0x00FFFFFF);
    }

    /* Fields well below logo — horizontal layout: [username] [password] [button] */
    int total_w = FIELD_W + FIELD_GAP + FIELD_W + FIELD_GAP + 100;
    int fx = cx - total_w / 2;
    int fy = s_fb_h * 3 / 4;  /* 75% down the screen */

    /* Lock mode indicator */
    if (s_is_lock) {
        int lock_tw = 6 * 8;
        draw_text_simple(cx - lock_tw / 2, fy - 22, "Locked", 0x00FF8888);
    }

    /* Username field */
    draw_fill_rect(&surf, fx, fy, FIELD_W, FIELD_H, 0x002A2A3E);
    if (s_focus == 0)
        draw_rect(&surf, fx, fy, FIELD_W, FIELD_H, 0x004488CC);
    if (s_user_len > 0)
        draw_text_simple(fx + 8, fy + 8, s_user_buf, 0x00FFFFFF);
    else if (s_focus != 0)
        draw_text_simple(fx + 8, fy + 8, "username", 0x00505060);
    int ux = fx + FIELD_W + FIELD_GAP;

    /* Password field */
    draw_fill_rect(&surf, ux, fy, FIELD_W, FIELD_H, 0x002A2A3E);
    if (s_focus == 1)
        draw_rect(&surf, ux, fy, FIELD_W, FIELD_H, 0x004488CC);
    if (s_pass_len > 0) {
        char stars[128];
        int i;
        for (i = 0; i < s_pass_len && i < 126; i++) stars[i] = '*';
        stars[i] = '\0';
        draw_text_simple(ux + 8, fy + 8, stars, 0x00FFFFFF);
    } else if (s_focus != 1) {
        draw_text_simple(ux + 8, fy + 8, "password", 0x00505060);
    }
    int bx = ux + FIELD_W + FIELD_GAP;

    /* Login/Unlock button */
    uint32_t btn_color = (s_focus == 2) ? 0x005577DD : 0x003A4A6A;
    draw_fill_rect(&surf, bx, fy, 100, FIELD_H, btn_color);
    const char *btn_text = s_is_lock ? "Unlock" : "Login";
    int btn_tw = (int)strlen(btn_text) * 8;
    draw_text_simple(bx + 50 - btn_tw / 2, fy + 8, btn_text, 0x00FFFFFF);

    /* Error message centered below */
    if (s_error[0]) {
        int err_tw = (int)strlen(s_error) * 8;
        draw_text_simple(cx - err_tw / 2, fy + FIELD_H + 16, s_error, 0x00FF4444);
    }

    blit_to_fb();
}

/* ---- Input handling -------------------------------------------------- */

static void
handle_key(char c)
{
    if (c == '\t') {
        /* Cycle focus: username -> password -> button -> username */
        s_focus = (s_focus + 1) % 3;
        /* Skip username in lock mode */
        if (s_is_lock && s_focus == 0) s_focus = 1;
        return;
    }

    if (c == '\n' || c == '\r') {
        /* Submit */
        s_focus = 2; /* highlight button */
        return; /* caller checks for enter */
    }

    /* Typing into focused field */
    if (s_focus == 0 && !s_is_lock) {
        /* Username field */
        if (c == '\x7f' || c == '\b') {
            if (s_user_len > 0) s_user_buf[--s_user_len] = '\0';
        } else if (c >= ' ' && s_user_len < 62) {
            s_user_buf[s_user_len++] = c;
            s_user_buf[s_user_len] = '\0';
        }
    } else if (s_focus == 1) {
        /* Password field */
        if (c == '\x7f' || c == '\b') {
            if (s_pass_len > 0) s_pass_buf[--s_pass_len] = '\0';
        } else if (c >= ' ' && s_pass_len < 126) {
            s_pass_buf[s_pass_len++] = c;
            s_pass_buf[s_pass_len] = '\0';
        }
    }
}

/* ---- Signal handler for lock (SIGUSR1) ------------------------------- */

static void
sigusr1_handler(int sig)
{
    (void)sig;
    s_locked = 1;
}

/* ---- Session spawn --------------------------------------------------- */

static pid_t
spawn_lumen(void)
{
    /* Use fork+execve instead of sys_spawn.  Lumen works from CLI
     * (fork+execve) but freezes from sys_spawn — likely a kernel bug
     * in sys_spawn's session/TTY setup.  fork+execve is the workaround. */
    pid_t pid = fork();
    if (pid == 0) {
        /* Child: grant caps AFTER fork (exec_caps not inherited by fork),
         * then exec lumen. */
        auth_grant_shell_caps();
        setenv("PATH", "/bin", 1);
        setenv("HOME", "/root", 1);
        setenv("USER", s_username, 1);
        setenv("TERM", "dumb", 1);
        char *argv[] = { "lumen", NULL };
        execve("/bin/lumen", argv, environ);
        _exit(127);
    }
    return pid;
}

/* ---- Authentication flow --------------------------------------------- */

static int
do_auth(void)
{
    char home[256], shell[256];
    int uid = 0, gid = 0;

    if (s_is_lock) {
        /* Lock mode: verify same user */
        if (auth_check(s_username, s_pass_buf, &uid, &gid,
                       home, (int)sizeof(home), shell, (int)sizeof(shell)) != 0)
            return -1;
        return 0;
    }

    /* Greeter mode: full auth */
    if (auth_check(s_user_buf, s_pass_buf, &uid, &gid,
                   home, (int)sizeof(home), shell, (int)sizeof(shell)) != 0)
        return -1;

    /* Save for lock screen and session */
    strncpy(s_username, s_user_buf, sizeof(s_username) - 1);
    s_uid = uid;
    s_gid = gid;

    auth_set_identity(uid, gid);
    auth_grant_shell_caps();

    return 0;
}

/* ---- Main ------------------------------------------------------------- */

int
main(void)
{
    /* Exit immediately unless booted in graphical mode.
     * Bastion is graphical only — if /proc/cmdline doesn't contain
     * "boot=graphical", exit immediately. */
    {
        int graphical = 0;
        int cfd = open("/proc/cmdline", O_RDONLY);
        if (cfd >= 0) {
            char cmd[128];
            int cn = (int)read(cfd, cmd, sizeof(cmd) - 1);
            close(cfd);
            if (cn > 0) {
                cmd[cn] = '\0';
                if (strstr(cmd, "boot=graphical"))
                    graphical = 1;
            }
        }
        if (!graphical)
            return 0;
    }

    /* Request capabilities from capd */
    auth_request_caps();

    /* Map framebuffer */
    memset(&s_fb_info, 0, sizeof(s_fb_info));
    if (syscall(SYS_FB_MAP, &s_fb_info) < 0) {
        dprintf(2, "bastion: sys_fb_map failed, sleeping 30s to avoid respawn storm\n");
        sleep(30);
        return 1;
    }
    s_fb = (uint32_t *)(uintptr_t)s_fb_info.addr;
    s_fb_w = (int)s_fb_info.width;
    s_fb_h = (int)s_fb_info.height;
    s_pitch_px = (int)(s_fb_info.pitch / (s_fb_info.bpp / 8));

    s_backbuf = malloc((size_t)s_pitch_px * s_fb_h * 4);
    if (!s_backbuf) {
        dprintf(2, "bastion: backbuffer alloc failed\n");
        return 1;
    }

    /* Load logo */
    load_logo();

    /* Snapshot current FB for crossfade into the login form.
     * On first boot, kernel logs may have flashed on screen — fill with
     * charcoal so the fade starts from a clean background.
     * On lock→greeter return, FB has Lumen's last frame (good to fade from). */
    {
        size_t fb_bytes = (size_t)s_pitch_px * s_fb_h * 4;
        size_t npx = (size_t)s_pitch_px * s_fb_h;
        s_saved_frame = malloc(fb_bytes);
        if (s_saved_frame) {
            /* Check if FB looks like kernel text mode (mostly black) */
            int dark_count = 0;
            for (int i = 0; i < 100 && i < (int)npx; i++)
                if ((s_fb[i] & 0x00F0F0F0) == 0) dark_count++;
            if (dark_count > 80) {
                /* FB is mostly black (kernel logs) — use solid charcoal */
                for (size_t i = 0; i < npx; i++)
                    s_saved_frame[i] = 0x00202030;
            } else {
                memcpy(s_saved_frame, s_fb, fb_bytes);
            }
            /* Also paint the FB charcoal immediately to hide any flash */
            for (size_t i = 0; i < npx; i++)
                s_fb[i] = 0x00202030;
        }
    }

    /* Raw keyboard mode */
    struct termios t_orig;
    tcgetattr(0, &t_orig);
    struct termios t_raw = t_orig;
    t_raw.c_lflag &= ~(unsigned)(ECHO | ICANON | ISIG);
    t_raw.c_cc[VMIN] = 0;
    t_raw.c_cc[VTIME] = 0;
    tcsetattr(0, TCSANOW, &t_raw);

    /* Install SIGUSR1 handler for lock screen */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigusr1_handler;
    sigaction(SIGUSR1, &sa, NULL);

    /* Open mouse (non-blocking) */
    int mouse_fd = open("/dev/mouse", O_RDONLY);

    /* ---- Greeter loop ------------------------------------------------ */
greeter:
    s_is_lock = 0;
    s_user_buf[0] = '\0'; s_user_len = 0;
    s_pass_buf[0] = '\0'; s_pass_len = 0;
    s_error[0] = '\0';
    s_focus = 0;
    s_locked = 0;

    /* Crossfade from previous screen (GRUB splash or Lumen) to login form */
    if (s_saved_frame) {
        draw_form();  /* renders login form to backbuf (blit_to_fb already called) */
        /* Undo the blit — we want to crossfade instead */
        memcpy(s_fb, s_saved_frame, (size_t)s_pitch_px * s_fb_h * 4);
        crossfade(20, 25);  /* 20 steps × 25ms = 500ms fade */
    }

    for (;;) {
        draw_form();

        /* Poll input */
        struct timespec ts = { 0, 16000000 }; /* 16ms */
        nanosleep(&ts, NULL);

        char c;
        while (read(0, &c, 1) == 1) {
            if (c == '\n' || c == '\r') {
                /* Submit */
                if (do_auth() == 0) {
                    /* Success — spawn Lumen */
                    s_lumen_pid = spawn_lumen();
                    if (s_lumen_pid <= 0) {
                        snprintf(s_error, sizeof(s_error), "Failed to start session");
                        continue;
                    }
                    goto session;
                } else {
                    snprintf(s_error, sizeof(s_error), "Invalid credentials");
                    s_pass_buf[0] = '\0'; s_pass_len = 0;
                }
            } else {
                handle_key(c);
            }
        }

        /* Read mouse (discard for now — no click handling in v1) */
        if (mouse_fd >= 0) {
            mouse_event_t me;
            while (read(mouse_fd, &me, sizeof(me)) == sizeof(me))
                ; /* drain */
        }
    }

session:
    /* Keep stdin + mouse open — needed for lock screen.
     * Lumen reads from PTY master fds, not stdin, so there's no
     * contention. Put terminal back to raw mode for lock screen input. */

    /* Wait for Lumen to exit */
    {
        int status;
        while (waitpid(s_lumen_pid, &status, 0) < 0) {
            if (errno != EINTR) break;
            if (s_locked) {
                /* Lock screen requested — re-enter raw mode for input */
                tcsetattr(0, TCSANOW, &t_raw);
                if (mouse_fd < 0)
                    mouse_fd = open("/dev/mouse", O_RDONLY);

                s_is_lock = 1;
                s_pass_buf[0] = '\0'; s_pass_len = 0;
                s_error[0] = '\0';
                s_focus = 1; /* password field */
                strncpy(s_user_buf, s_username, sizeof(s_user_buf) - 1);
                s_user_len = (int)strlen(s_user_buf);

                /* Draw lock screen and handle unlock */
                for (;;) {
                    draw_form();
                    struct timespec ts = { 0, 16000000 };
                    nanosleep(&ts, NULL);

                    char c;
                    while (read(0, &c, 1) == 1) {
                        if (c == '\n' || c == '\r') {
                            if (do_auth() == 0) {
                                /* Unlock — resume Lumen */
                                kill(s_lumen_pid, SIGUSR2);
                                s_locked = 0;
                                goto session;
                            } else {
                                snprintf(s_error, sizeof(s_error), "Invalid credentials");
                                s_pass_buf[0] = '\0'; s_pass_len = 0;
                            }
                        } else {
                            handle_key(c);
                        }
                    }

                    if (mouse_fd >= 0) {
                        mouse_event_t me;
                        while (read(mouse_fd, &me, sizeof(me)) == sizeof(me))
                            ;
                    }
                }
            }
        }
    }

    /* Lumen exited — reopen input devices and show greeter again */
    open("/dev/kbd", O_RDONLY);  /* fd 0 — stdin/keyboard */
    mouse_fd = open("/dev/mouse", O_RDONLY);
    tcgetattr(0, &t_orig);
    t_raw = t_orig;
    t_raw.c_lflag &= ~(unsigned)(ECHO | ICANON | ISIG);
    t_raw.c_cc[VMIN] = 0;
    t_raw.c_cc[VTIME] = 0;
    tcsetattr(0, TCSANOW, &t_raw);
    goto greeter;
}
