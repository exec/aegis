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

void terminal_write(glyph_window_t *win, const char *data, int len)
{
    term_priv_t *tp = win->priv;

    for (int i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)data[i];

        if (ch == '\n') {
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
        /* other bytes (ANSI escapes, etc.) silently dropped */
    }

    glyph_window_mark_all_dirty(win);
}

glyph_window_t *terminal_create(int cols, int rows, int *master_fd_out)
{
    int master_fd = -1, pts_num = -1, slave_fd = -1;
    char slave_path[32];
    const char *fail_reason = NULL;

    /* Create glyph window */
    int client_w = cols * FONT_W;
    int client_h = rows * FONT_H;
    glyph_window_t *win = glyph_window_create("Terminal", client_w, client_h);
    if (!win)
        return NULL;
    win->on_key = term_on_key;
    win->on_render = term_render_content;

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

    /* Open PTY master */
    master_fd = open("/dev/ptmx", O_RDWR);
    if (master_fd < 0) {
        fail_reason = "open /dev/ptmx failed";
        goto pty_failed;
    }

    if (ioctl(master_fd, 0x80045430 /* TIOCGPTN */, &pts_num) < 0) {
        fail_reason = "TIOCGPTN failed";
        goto pty_failed;
    }

    /* Unlock slave */
    {
        int unlock = 0;
        ioctl(master_fd, 0x40045431 /* TIOCSPTLCK */, &unlock);
    }

    /* Build slave path */
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

    /* Open slave fd — sys_spawn will dup it to child's fd 0/1/2 */
    slave_fd = open(slave_path, O_RDWR);
    if (slave_fd < 0) {
        fail_reason = "open slave failed";
        goto pty_failed;
    }

    /* Set master non-blocking for polling */
    fcntl(master_fd, F_SETFL, O_NONBLOCK);

    /* Spawn shell via sys_spawn — NO FORK, no page copy, instant. */
    {
        char *argv[] = {"-stsh", NULL};  /* leading '-' = login shell */
        char *envp[] = {"PATH=/bin", "HOME=/root", "TERM=dumb", NULL};

        long pid = syscall(SYS_SPAWN, "/bin/stsh", argv, envp, slave_fd, 0);
        if (pid < 0) {
            fail_reason = "sys_spawn failed";
            goto pty_failed;
        }
    }

    /* Parent doesn't need the slave fd */
    close(slave_fd);

    /* Success */
    tp->master_fd = master_fd;
    *master_fd_out = master_fd;
    term_render_content(win);
    return win;

pty_failed:
    if (slave_fd >= 0)
        close(slave_fd);
    if (master_fd >= 0)
        close(master_fd);
    tp->master_fd = -1;
    *master_fd_out = -1;

    term_grid_puts(tp, "Terminal: PTY unavailable\n");
    if (fail_reason)
        term_grid_puts(tp, fail_reason);
    term_grid_puts(tp, "\n\nLumen is running without a shell.\n");
    term_grid_puts(tp, "Reboot into text mode for a shell.");

    term_render_content(win);
    return win;
}
