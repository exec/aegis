/* terminal.c -- PTY-based terminal emulator window for Lumen
 *
 * Uses sys_spawn (syscall 514) to create the shell process WITHOUT fork.
 * This avoids the 30+ second stall on bare metal caused by fork's eager
 * page copy of lumen's ~3000 pages + vmm_window_lock contention during
 * the child's execve. sys_spawn creates a fresh PML4 with the ELF loaded
 * directly — no page copy, no lock contention. */

#include "terminal.h"
#include "compositor.h"
#include <glyph.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>

/* sys_spawn: create process from ELF path without fork.
 * syscall(514, path, argv, envp, stdio_fd, cap_mask) */
#define SYS_SPAWN 514

typedef struct {
    int master_fd;  /* -1 if PTY failed */
    int cols, rows;
    int cx, cy;
    char *grid;
    /* ANSI escape parser state */
    int esc_state;      /* 0=normal, 1=got ESC, 2=got CSI (ESC[) */
    int esc_params[4];
    int esc_nparam;
} term_priv_t;

static void term_scroll(term_priv_t *tp)
{
    memmove(tp->grid, tp->grid + tp->cols,
            (unsigned)(tp->rows - 1) * (unsigned)tp->cols);
    memset(tp->grid + (tp->rows - 1) * tp->cols, ' ', (unsigned)tp->cols);
    tp->cy = tp->rows - 1;
}

static void term_render_content(glyph_window_t *win)
{
    term_priv_t *tp = win->priv;

    surface_t *s = &win->surface;
    int ox = GLYPH_BORDER_WIDTH;
    int oy = GLYPH_BORDER_WIDTH + GLYPH_TITLEBAR_HEIGHT;

    draw_fill_rect(s, ox, oy, win->client_w, win->client_h, C_TERM_BG);

    for (int r = 0; r < tp->rows; r++) {
        for (int c = 0; c < tp->cols; c++) {
            char ch = tp->grid[r * tp->cols + c];
            if (ch != ' ')
                draw_char(s, ox + c * FONT_W, oy + r * FONT_H,
                          ch, C_TERM_FG, C_TERM_BG);
        }
    }

    if (tp->master_fd >= 0)
        draw_fill_rect(s, ox + tp->cx * FONT_W, oy + tp->cy * FONT_H,
                       FONT_W, FONT_H, C_TERM_FG);
}

static void term_on_key(glyph_window_t *self, char key)
{
    term_priv_t *tp = self->priv;
    if (tp->master_fd >= 0)
        write(tp->master_fd, &key, 1);
}

static void term_grid_puts(term_priv_t *tp, const char *msg)
{
    while (*msg) {
        unsigned char ch = (unsigned char)*msg++;
        if (ch == '\n') {
            tp->cy++;
            tp->cx = 0;
            if (tp->cy >= tp->rows)
                term_scroll(tp);
        } else if (ch >= 32 && ch <= 126) {
            tp->grid[tp->cy * tp->cols + tp->cx] = (char)ch;
            tp->cx++;
            if (tp->cx >= tp->cols) {
                tp->cx = 0;
                tp->cy++;
                if (tp->cy >= tp->rows)
                    term_scroll(tp);
            }
        }
    }
}

/* Handle a completed CSI sequence: ESC [ params... final_char */
static void term_csi(term_priv_t *tp, int final)
{
    int n = (tp->esc_nparam > 0) ? tp->esc_params[0] : 0;

    switch (final) {
    case 'A': /* CUU — cursor up */
        if (n < 1) n = 1;
        tp->cy -= n;
        if (tp->cy < 0) tp->cy = 0;
        break;
    case 'B': /* CUD — cursor down */
        if (n < 1) n = 1;
        tp->cy += n;
        if (tp->cy >= tp->rows) tp->cy = tp->rows - 1;
        break;
    case 'C': /* CUF — cursor forward */
        if (n < 1) n = 1;
        tp->cx += n;
        if (tp->cx >= tp->cols) tp->cx = tp->cols - 1;
        break;
    case 'D': /* CUB — cursor back */
        if (n < 1) n = 1;
        tp->cx -= n;
        if (tp->cx < 0) tp->cx = 0;
        break;
    case 'H': /* CUP — cursor position */
    case 'f': {
        int row = (tp->esc_nparam > 0) ? tp->esc_params[0] : 1;
        int col = (tp->esc_nparam > 1) ? tp->esc_params[1] : 1;
        if (row < 1) row = 1;
        if (col < 1) col = 1;
        tp->cy = row - 1;
        tp->cx = col - 1;
        if (tp->cy >= tp->rows) tp->cy = tp->rows - 1;
        if (tp->cx >= tp->cols) tp->cx = tp->cols - 1;
        break;
    }
    case 'J': /* ED — erase in display */
        if (n == 2) {
            /* Clear entire screen */
            memset(tp->grid, ' ', (unsigned)(tp->cols * tp->rows));
            tp->cx = 0;
            tp->cy = 0;
        } else if (n == 0) {
            /* Clear from cursor to end */
            int pos = tp->cy * tp->cols + tp->cx;
            int total = tp->cols * tp->rows;
            if (pos < total)
                memset(tp->grid + pos, ' ', (unsigned)(total - pos));
        }
        break;
    case 'K': /* EL — erase in line */
        if (n == 0) {
            /* Clear from cursor to end of line */
            int pos = tp->cy * tp->cols + tp->cx;
            int end = (tp->cy + 1) * tp->cols;
            if (pos < end)
                memset(tp->grid + pos, ' ', (unsigned)(end - pos));
        } else if (n == 2) {
            /* Clear entire line */
            memset(tp->grid + tp->cy * tp->cols, ' ', (unsigned)tp->cols);
        }
        break;
    case 'm': /* SGR — ignore (no color support yet) */
        break;
    default:
        break;
    }
}

void terminal_write(glyph_window_t *win, const char *data, int len)
{
    term_priv_t *tp = win->priv;

    for (int i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)data[i];

        /* ANSI escape sequence parser */
        if (tp->esc_state == 1) {
            /* Got ESC, expect '[' for CSI */
            if (ch == '[') {
                tp->esc_state = 2;
                tp->esc_nparam = 0;
                tp->esc_params[0] = 0;
                tp->esc_params[1] = 0;
            } else {
                tp->esc_state = 0; /* unknown ESC seq, drop */
            }
            continue;
        }
        if (tp->esc_state == 2) {
            /* Inside CSI — collect params and final byte */
            if (ch >= '0' && ch <= '9') {
                if (tp->esc_nparam == 0) tp->esc_nparam = 1;
                tp->esc_params[tp->esc_nparam - 1] =
                    tp->esc_params[tp->esc_nparam - 1] * 10 + (ch - '0');
            } else if (ch == ';') {
                if (tp->esc_nparam < 4) {
                    tp->esc_params[tp->esc_nparam] = 0;
                    tp->esc_nparam++;
                }
            } else if (ch >= 0x40 && ch <= 0x7E) {
                /* Final byte — execute */
                term_csi(tp, ch);
                tp->esc_state = 0;
            } else {
                tp->esc_state = 0; /* unexpected byte, abort */
            }
            continue;
        }

        /* Normal character processing */
        if (ch == 0x1B) {
            tp->esc_state = 1;
            continue;   /* ESC byte must never fall through */
        } else if (ch == '\n') {
            tp->cy++;
            if (tp->cy >= tp->rows)
                term_scroll(tp);
        } else if (ch == '\r') {
            tp->cx = 0;
        } else if (ch == '\b' || ch == 127) {
            if (tp->cx > 0)
                tp->cx--;
        } else if (ch == '\t') {
            tp->cx = (tp->cx + 8) & ~7;
            if (tp->cx >= tp->cols) {
                tp->cx = 0;
                tp->cy++;
                if (tp->cy >= tp->rows)
                    term_scroll(tp);
            }
        } else if (ch >= 32 && ch <= 126) {
            tp->grid[tp->cy * tp->cols + tp->cx] = (char)ch;
            tp->cx++;
            if (tp->cx >= tp->cols) {
                tp->cx = 0;
                tp->cy++;
                if (tp->cy >= tp->rows)
                    term_scroll(tp);
            }
        }
    }

    glyph_window_mark_all_dirty(win);
}

/* ---- Dropdown terminal (chromeless, rounded bottom corners) ---- */

#define DROPDOWN_CORNER_R 12

/* Check if (px, py) is outside the rounded bottom corners.
 * Total surface is surf_w x surf_h. Only the bottom two corners are rounded. */
static int
outside_bottom_corner(int px, int py, int surf_w, int surf_h, int r)
{
    /* Only check the bottom strip (last r rows) */
    if (py < surf_h - r)
        return 0;

    int dy = py - (surf_h - r - 1);

    /* Bottom-left corner */
    if (px < r) {
        int dx = r - px;
        if (dx * dx + dy * dy > r * r)
            return 1;
    }

    /* Bottom-right corner */
    if (px >= surf_w - r) {
        int dx = px - (surf_w - r - 1);
        if (dx * dx + dy * dy > r * r)
            return 1;
    }

    return 0;
}

static void
dropdown_render_content(glyph_window_t *win)
{
    term_priv_t *tp = win->priv;
    surface_t *s = &win->surface;

    /* Fill entire surface with terminal background */
    draw_fill_rect(s, 0, 0, win->surf_w, win->surf_h, C_TERM_BG);

    /* Draw a subtle accent line at the very top */
    draw_fill_rect(s, 0, 0, win->surf_w, 2, C_ACCENT);

    /* Terminal content starts at (4, 4) with small padding */
    int pad_x = 4;
    int pad_y = 4;

    for (int r = 0; r < tp->rows; r++) {
        for (int c = 0; c < tp->cols; c++) {
            char ch = tp->grid[r * tp->cols + c];
            if (ch != ' ')
                draw_char(s, pad_x + c * FONT_W, pad_y + r * FONT_H,
                          ch, C_TERM_FG, C_TERM_BG);
        }
    }

    /* Cursor block */
    if (tp->master_fd >= 0)
        draw_fill_rect(s, pad_x + tp->cx * FONT_W, pad_y + tp->cy * FONT_H,
                       FONT_W, FONT_H, C_TERM_FG);

    /* Round the bottom corners by painting the background color
     * over pixels that fall outside the corner arcs. */
    for (int y = win->surf_h - DROPDOWN_CORNER_R; y < win->surf_h; y++) {
        for (int x = 0; x < win->surf_w; x++) {
            if (outside_bottom_corner(x, y, win->surf_w, win->surf_h,
                                      DROPDOWN_CORNER_R))
                draw_px(s, x, y, C_BG1);  /* desktop bg color */
        }
    }
}

/* Spawn the shell process for a terminal (shared between normal + dropdown) */
static int
spawn_shell(int slave_fd)
{
    char *argv[] = {"-stsh", NULL};  /* leading '-' = login shell */
    char *envp[] = {"PATH=/bin", "HOME=/root", "TERM=dumb", NULL};

    long pid = syscall(SYS_SPAWN, "/bin/stsh", argv, envp, slave_fd, 0);
    return (int)pid;
}

/* Open a PTY pair and spawn a shell. Returns master_fd or -1 on failure.
 * If fail_reason is non-NULL, sets it to a description. */
static int
pty_open_and_spawn(const char **fail_reason_out)
{
    int master_fd = -1, pts_num = -1, slave_fd = -1;
    char slave_path[32];
    const char *fail_reason = NULL;

    master_fd = open("/dev/ptmx", O_RDWR);
    if (master_fd < 0) {
        fail_reason = "open /dev/ptmx failed";
        goto fail;
    }

    if (ioctl(master_fd, 0x80045430 /* TIOCGPTN */, &pts_num) < 0) {
        fail_reason = "TIOCGPTN failed";
        goto fail;
    }

    {
        int unlock = 0;
        ioctl(master_fd, 0x40045431 /* TIOCSPTLCK */, &unlock);
    }

    {
        const char *prefix = "/dev/pts/";
        int j = 0;
        while (*prefix)
            slave_path[j++] = *prefix++;
        if (pts_num == 0) {
            slave_path[j++] = '0';
        } else {
            char digits[10];
            int nd = 0, n = pts_num;
            while (n > 0) {
                digits[nd++] = '0' + (n % 10);
                n /= 10;
            }
            for (int k = nd - 1; k >= 0; k--)
                slave_path[j++] = digits[k];
        }
        slave_path[j] = '\0';
    }

    slave_fd = open(slave_path, O_RDWR);
    if (slave_fd < 0) {
        fail_reason = "open slave failed";
        goto fail;
    }

    fcntl(master_fd, F_SETFL, O_NONBLOCK);

    {
        long pid = spawn_shell(slave_fd);
        if (pid < 0) {
            fail_reason = "sys_spawn failed";
            goto fail;
        }
    }

    close(slave_fd);
    return master_fd;

fail:
    if (slave_fd >= 0)
        close(slave_fd);
    if (master_fd >= 0)
        close(master_fd);
    if (fail_reason_out)
        *fail_reason_out = fail_reason;
    return -1;
}

glyph_window_t *
terminal_create_dropdown(int screen_w, int screen_h, int *master_fd_out)
{
    int margin = 20;
    int dd_w = screen_w - 2 * margin;
    int dd_h = screen_h * 45 / 100;

    /* Compute terminal grid size from pixel dimensions with padding */
    int pad_x = 4;
    int pad_y = 4;
    int cols = (dd_w - 2 * pad_x) / FONT_W;
    int rows = (dd_h - 2 * pad_y) / FONT_H;
    if (cols < 10) cols = 10;
    if (rows < 4) rows = 4;

    /* Allocate terminal private data */
    term_priv_t *tp = calloc(1, sizeof(*tp));
    if (!tp) {
        *master_fd_out = -1;
        return NULL;
    }
    tp->master_fd = -1;
    tp->cols = cols;
    tp->rows = rows;
    tp->grid = calloc((unsigned)(cols * rows), 1);
    if (!tp->grid) {
        free(tp);
        *master_fd_out = -1;
        return NULL;
    }
    memset(tp->grid, ' ', (unsigned)(cols * rows));

    /* Create a glyph window — we will override the chrome with our renderer.
     * Use client_w/client_h = full dropdown size (the chrome adds border+titlebar
     * but our on_render overwrites everything). */
    glyph_window_t *win = glyph_window_create("Dropdown", dd_w, dd_h);
    if (!win) {
        free(tp->grid);
        free(tp);
        *master_fd_out = -1;
        return NULL;
    }

    win->on_key = term_on_key;
    win->on_render = dropdown_render_content;
    win->priv = tp;
    win->closeable = 0;
    win->frosted = 1;
    win->visible = 0;  /* starts hidden */

    /* Position: centered horizontally at top of screen */
    win->x = margin;
    win->y = 0;

    /* Open PTY and spawn shell */
    const char *fail_reason = NULL;
    int mfd = pty_open_and_spawn(&fail_reason);
    if (mfd >= 0) {
        tp->master_fd = mfd;
        *master_fd_out = mfd;
        win->tag = mfd;  /* for PTY polling in main loop */
    } else {
        *master_fd_out = -1;
        term_grid_puts(tp, "Dropdown: PTY unavailable\n");
        if (fail_reason)
            term_grid_puts(tp, fail_reason);
    }

    dropdown_render_content(win);
    return win;
}

glyph_window_t *terminal_create(int cols, int rows, int *master_fd_out)
{
    /* Create glyph window */
    int client_w = cols * FONT_W;
    int client_h = rows * FONT_H;
    glyph_window_t *win = glyph_window_create("Terminal", client_w, client_h);
    if (!win)
        return NULL;
    win->on_key = term_on_key;
    win->on_render = term_render_content;
    win->frosted = 1;

    /* Allocate terminal private data */
    term_priv_t *tp = calloc(1, sizeof(*tp));
    if (!tp) {
        glyph_window_destroy(win);
        return NULL;
    }
    tp->master_fd = -1;
    tp->cols = cols;
    tp->rows = rows;
    tp->grid = calloc((unsigned)(cols * rows), 1);
    if (!tp->grid) {
        free(tp);
        glyph_window_destroy(win);
        return NULL;
    }
    memset(tp->grid, ' ', (unsigned)(cols * rows));
    win->priv = tp;

    /* Open PTY and spawn shell */
    const char *fail_reason = NULL;
    int mfd = pty_open_and_spawn(&fail_reason);
    if (mfd >= 0) {
        tp->master_fd = mfd;
        *master_fd_out = mfd;
    } else {
        *master_fd_out = -1;
        term_grid_puts(tp, "Terminal: PTY unavailable\n");
        if (fail_reason)
            term_grid_puts(tp, fail_reason);
        term_grid_puts(tp, "\n\nLumen is running without a shell.\n");
        term_grid_puts(tp, "Reboot into text mode for a shell.");
    }

    term_render_content(win);
    return win;
}
