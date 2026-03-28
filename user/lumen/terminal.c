/* terminal.c -- PTY-based terminal emulator window for Lumen */

#include "terminal.h"
#include "draw.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>

typedef struct {
    int master_fd;
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

static void term_render(window_t *win)
{
    term_priv_t *tp = win->priv;
    surface_t s;

    s.buf = win->pixels;
    s.w = win->w;
    s.h = win->h;
    s.pitch = win->w;

    draw_fill_rect(&s, 0, 0, s.w, s.h, C_TERM_BG);

    for (int r = 0; r < tp->rows; r++) {
        for (int c = 0; c < tp->cols; c++) {
            char ch = tp->grid[r * tp->cols + c];
            if (ch != ' ')
                draw_char(&s, c * FONT_W, r * FONT_H, ch, C_TERM_FG, C_TERM_BG);
        }
    }

    /* cursor: filled rectangle at current position */
    draw_fill_rect(&s, tp->cx * FONT_W, tp->cy * FONT_H,
                   FONT_W, FONT_H, C_TERM_FG);
}

static void term_on_key(window_t *self, char key)
{
    term_priv_t *tp = self->priv;
    write(tp->master_fd, &key, 1);
}

static void term_on_draw(window_t *self)
{
    term_render(self);
}

void terminal_write(window_t *win, const char *data, int len)
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
}

window_t *terminal_create(int cols, int rows, int *master_fd_out)
{
    int master_fd, pts_num, slave_fd;
    char slave_path[32];
    window_t *win;
    term_priv_t *tp;
    pid_t pid;

    /* open PTY master */
    master_fd = open("/dev/ptmx", O_RDWR);
    if (master_fd < 0)
        return NULL;

    /* get slave number */
    if (ioctl(master_fd, 0x80045430 /* TIOCGPTN */, &pts_num) < 0) {
        close(master_fd);
        return NULL;
    }

    /* build slave path */
    {
        const char *prefix = "/dev/pts/";
        int j = 0;
        while (*prefix)
            slave_path[j++] = *prefix++;
        /* convert pts_num to string */
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

    /* allocate window */
    win = calloc(1, sizeof(*win));
    if (!win) {
        close(master_fd);
        return NULL;
    }
    win->w = cols * FONT_W;
    win->h = rows * FONT_H;
    win->pixels = calloc((unsigned)(win->w * win->h), 4);
    if (!win->pixels) {
        free(win);
        close(master_fd);
        return NULL;
    }
    memcpy(win->title, "Terminal", 9);
    win->visible = 1;
    win->closeable = 1;
    win->on_key = term_on_key;
    win->on_draw = term_on_draw;

    /* allocate private data */
    tp = calloc(1, sizeof(*tp));
    if (!tp) {
        free(win->pixels);
        free(win);
        close(master_fd);
        return NULL;
    }
    tp->master_fd = master_fd;
    tp->cols = cols;
    tp->rows = rows;
    tp->cx = 0;
    tp->cy = 0;
    tp->grid = calloc((unsigned)(cols * rows), 1);
    if (!tp->grid) {
        free(tp);
        free(win->pixels);
        free(win);
        close(master_fd);
        return NULL;
    }
    memset(tp->grid, ' ', (unsigned)(cols * rows));
    win->priv = tp;

    /* set master non-blocking */
    fcntl(master_fd, F_SETFL, O_NONBLOCK);

    /* fork child shell */
    pid = fork();
    if (pid < 0) {
        free(tp->grid);
        free(tp);
        free(win->pixels);
        free(win);
        close(master_fd);
        return NULL;
    }

    if (pid == 0) {
        /* child */
        close(master_fd);
        setsid();
        slave_fd = open(slave_path, O_RDWR);
        if (slave_fd < 0)
            _exit(1);
        dup2(slave_fd, 0);
        dup2(slave_fd, 1);
        dup2(slave_fd, 2);
        if (slave_fd > 2)
            close(slave_fd);

        char *argv[] = {"/bin/oksh", NULL};
        char *envp[] = {
            "TERM=dumb",
            "HOME=/root",
            "PATH=/bin",
            NULL
        };
        execve("/bin/oksh", argv, envp);
        _exit(1);
    }

    /* parent */
    *master_fd_out = master_fd;
    return win;
}
