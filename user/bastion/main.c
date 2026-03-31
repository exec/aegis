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
    if (s_logo_w <= 0 || s_logo_h <= 0 || s_logo_w > 800 || s_logo_h > 400) {
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
    int x0 = cx - s_logo_w / 2;
    for (int ly = 0; ly < s_logo_h; ly++) {
        int dy = y + ly;
        if (dy < 0 || dy >= s_fb_h) continue;
        for (int lx = 0; lx < s_logo_w; lx++) {
            int dx = x0 + lx;
            if (dx < 0 || dx >= s_fb_w) continue;
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

/* ---- Login form ------------------------------------------------------ */

#define FORM_W      360
#define FIELD_H     32
#define FIELD_GAP   12
#define BTN_H       36

static char s_user_buf[64];
static char s_pass_buf[128];
static int  s_user_len, s_pass_len;
static int  s_focus; /* 0=username, 1=password, 2=button */
static char s_error[128];
static int  s_is_lock; /* 1 = lock screen mode */

static void
draw_form(void)
{
    fill_bg();

    int cx = s_fb_w / 2;
    int form_x = cx - FORM_W / 2;

    /* Logo or fallback text */
    int logo_y = s_fb_h / 4 - (s_logo_h > 0 ? s_logo_h / 2 : 20);
    if (s_logo_pixels) {
        draw_logo(cx, logo_y);
    } else {
        draw_text_simple(cx - 5 * 8 / 2, logo_y, "AEGIS", 0x00FFFFFF);
    }

    int form_y = s_fb_h / 2 - 60;
    surface_t surf = { .buf = s_backbuf, .w = s_fb_w, .h = s_fb_h, .pitch = s_pitch_px };

    /* Status text for lock mode */
    if (s_is_lock) {
        draw_text_simple(cx - 3 * 8, form_y - 30, "Locked", 0x00FF8888);
    }

    /* Username label + field */
    draw_text_simple(form_x, form_y, "Username", 0x00808090);
    form_y += 18;
    draw_fill_rect(&surf, form_x, form_y, FORM_W, FIELD_H, 0x002A2A3E);
    if (s_focus == 0)
        draw_rect(&surf, form_x, form_y, FORM_W, FIELD_H, 0x004488CC);
    draw_text_simple(form_x + 8, form_y + 8, s_user_buf, 0x00FFFFFF);
    form_y += FIELD_H + FIELD_GAP;

    /* Password label + field */
    draw_text_simple(form_x, form_y, "Password", 0x00808090);
    form_y += 18;
    draw_fill_rect(&surf, form_x, form_y, FORM_W, FIELD_H, 0x002A2A3E);
    if (s_focus == 1)
        draw_rect(&surf, form_x, form_y, FORM_W, FIELD_H, 0x004488CC);
    /* Show asterisks */
    {
        char stars[128];
        int i;
        for (i = 0; i < s_pass_len && i < 126; i++) stars[i] = '*';
        stars[i] = '\0';
        draw_text_simple(form_x + 8, form_y + 8, stars, 0x00FFFFFF);
    }
    form_y += FIELD_H + FIELD_GAP;

    /* Button */
    uint32_t btn_color = (s_focus == 2) ? 0x005577DD : 0x004466BB;
    draw_fill_rect(&surf, form_x, form_y, FORM_W, BTN_H, btn_color);
    const char *btn_text = s_is_lock ? "Unlock" : "Login";
    int btn_tw = (int)strlen(btn_text) * 8;
    draw_text_simple(form_x + FORM_W / 2 - btn_tw / 2, form_y + 10, btn_text, 0x00FFFFFF);
    form_y += BTN_H + FIELD_GAP;

    /* Error message */
    if (s_error[0])
        draw_text_simple(form_x, form_y, s_error, 0x00FF4444);

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
    char *argv[] = { "lumen", NULL };
    char home_env[128], user_env[128];
    snprintf(home_env, sizeof(home_env), "HOME=/root");
    snprintf(user_env, sizeof(user_env), "USER=%s", s_username);
    char *envp[] = { "PATH=/bin", home_env, user_env, "TERM=dumb", NULL };

    long pid = syscall(SYS_SPAWN, (long)"/bin/lumen", (long)argv,
                       (long)envp, (long)-1, 0L);
    return (pid_t)pid;
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
        dprintf(2, "bastion: sys_fb_map failed\n");
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
    /* Release input resources so Lumen has exclusive access to
     * keyboard, mouse, and the console TTY. Bastion is blocked in
     * waitpid and doesn't need them until the lock screen. */
    tcsetattr(0, TCSANOW, &t_orig);  /* restore terminal to cooked mode */
    if (mouse_fd >= 0) { close(mouse_fd); mouse_fd = -1; }
    close(0);  /* release kbd so Lumen gets it exclusively */

    /* Wait for Lumen to exit */
    {
        int status;
        while (waitpid(s_lumen_pid, &status, 0) < 0) {
            if (errno != EINTR) break;
            if (s_locked) {
                /* Lock screen requested */
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
