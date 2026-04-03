/* terminal.c -- PTY-based terminal emulator window for Lumen
 *
 * Uses sys_spawn (syscall 514) to create the shell process WITHOUT fork.
 * This avoids the 30+ second stall on bare metal caused by fork's eager
 * page copy of lumen's ~3000 pages + vmm_window_lock contention during
 * the child's execve. sys_spawn creates a fresh PML4 with the ELF loaded
 * directly — no page copy, no lock contention. */

#include "terminal.h"
#include "compositor.h"
#include "font.h"
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

#define SCROLLBACK_LINES 500  /* lines of history above visible area */

#define SEL_HIGHLIGHT 0x004488CC  /* selection highlight color */
#define SEL_ALPHA     80          /* highlight opacity (0-255) */

typedef struct {
    int master_fd;  /* -1 if PTY failed */
    int cols, rows;
    int cx, cy;     /* cursor position (relative to visible area bottom) */
    int cell_w, cell_h; /* character cell size (TTF or bitmap fallback) */
    char *grid;     /* ring buffer: total_rows * cols */
    int total_rows; /* rows + SCROLLBACK_LINES */
    int scroll_top; /* index of first visible line in ring buffer (bottom view) */
    int scroll_offset; /* how far user has scrolled back (0 = bottom) */
    /* ANSI escape parser state */
    int esc_state;      /* 0=normal, 1=got ESC, 2=got CSI (ESC[) */
    int esc_params[4];
    int esc_nparam;
    /* Selection state */
    int sel_active;     /* 1 if selection in progress */
    int sel_start_col, sel_start_row;  /* start of selection (visible grid coords) */
    int sel_end_col, sel_end_row;      /* end of selection (visible grid coords) */
    int sel_done;       /* 1 if selection completed (mouse released) */
    /* Padding offsets (for dropdown vs normal) */
    int pad_x, pad_y;
    int is_dropdown;
} term_priv_t;

/* Access a cell in the ring buffer.
 * vis_row is 0..rows-1 (top of visible area = 0).
 * scroll_offset shifts the view up into history. */
static inline char *
grid_cell(term_priv_t *tp, int vis_row, int col)
{
    int ring_row = (tp->scroll_top - tp->scroll_offset + vis_row) % tp->total_rows;
    if (ring_row < 0)
        ring_row += tp->total_rows;
    return &tp->grid[ring_row * tp->cols + col];
}

/* Access a cell at the cursor's absolute position (ignoring scroll_offset).
 * Used by the output path which always writes at the bottom. */
static inline char *
grid_cell_abs(term_priv_t *tp, int vis_row, int col)
{
    int ring_row = (tp->scroll_top + vis_row) % tp->total_rows;
    if (ring_row < 0)
        ring_row += tp->total_rows;
    return &tp->grid[ring_row * tp->cols + col];
}

static void term_scroll(term_priv_t *tp)
{
    /* Advance scroll_top: the old top line scrolls into history,
     * and the new bottom line is cleared. */
    tp->scroll_top = (tp->scroll_top + 1) % tp->total_rows;
    /* Clear the new bottom row (which is now at visible row rows-1) */
    int new_bottom = (tp->scroll_top + tp->rows - 1) % tp->total_rows;
    memset(&tp->grid[new_bottom * tp->cols], ' ', (unsigned)tp->cols);
    tp->cy = tp->rows - 1;
}

/* Get padding offsets for a window (dropdown vs normal) */
static void
get_padding(term_priv_t *tp, int *ox, int *oy)
{
    *ox = tp->pad_x;
    *oy = tp->pad_y;
}

/* Normalize selection so start is before end (row-major) */
static void
sel_normalize(term_priv_t *tp, int *sr, int *sc, int *er, int *ec)
{
    int s_r = tp->sel_start_row, s_c = tp->sel_start_col;
    int e_r = tp->sel_end_row, e_c = tp->sel_end_col;

    if (s_r > e_r || (s_r == e_r && s_c > e_c)) {
        *sr = e_r; *sc = e_c;
        *er = s_r; *ec = s_c;
    } else {
        *sr = s_r; *sc = s_c;
        *er = e_r; *ec = e_c;
    }
}

/* Check if a cell (r, c) is within the selection range */
static int
cell_selected(term_priv_t *tp, int r, int c)
{
    if (!tp->sel_active)
        return 0;

    int sr, sc, er, ec;
    sel_normalize(tp, &sr, &sc, &er, &ec);

    if (r < sr || r > er)
        return 0;
    if (r == sr && r == er)
        return c >= sc && c <= ec;
    if (r == sr)
        return c >= sc;
    if (r == er)
        return c <= ec;
    return 1; /* middle row, fully selected */
}

static void term_render_content(glyph_window_t *win)
{
    term_priv_t *tp = win->priv;

    surface_t *s = &win->surface;
    int ox, oy;
    get_padding(tp, &ox, &oy);
    int cw = tp->cell_w;
    int ch = tp->cell_h;

    draw_fill_rect(s, ox, oy, win->client_w, win->client_h, C_TERM_BG);

    for (int r = 0; r < tp->rows; r++) {
        for (int c = 0; c < tp->cols; c++) {
            char gc = *grid_cell(tp, r, c);

            /* Draw selection highlight */
            if (cell_selected(tp, r, c)) {
                draw_blend_rect(s, ox + c * cw, oy + r * ch,
                                cw, ch, SEL_HIGHLIGHT, SEL_ALPHA);
            }

            if (gc != ' ') {
                if (g_font_mono)
                    font_draw_char(s, g_font_mono, 16,
                                   ox + c * cw, oy + r * ch,
                                   gc, C_TERM_FG);
                else
                    draw_char(s, ox + c * cw, oy + r * ch,
                              gc, C_TERM_FG, C_TERM_BG);
            }
        }
    }

    /* Draw cursor only when viewing the bottom (no scroll offset) */
    if (tp->master_fd >= 0 && tp->scroll_offset == 0)
        draw_fill_rect(s, ox + tp->cx * cw, oy + tp->cy * ch,
                       cw, ch, C_TERM_FG);
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
            *grid_cell_abs(tp, tp->cy, tp->cx) = (char)ch;
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
    case 'A': /* CUU -- cursor up */
        if (n < 1) n = 1;
        tp->cy -= n;
        if (tp->cy < 0) tp->cy = 0;
        break;
    case 'B': /* CUD -- cursor down */
        if (n < 1) n = 1;
        tp->cy += n;
        if (tp->cy >= tp->rows) tp->cy = tp->rows - 1;
        break;
    case 'C': /* CUF -- cursor forward */
        if (n < 1) n = 1;
        tp->cx += n;
        if (tp->cx >= tp->cols) tp->cx = tp->cols - 1;
        break;
    case 'D': /* CUB -- cursor back */
        if (n < 1) n = 1;
        tp->cx -= n;
        if (tp->cx < 0) tp->cx = 0;
        break;
    case 'H': /* CUP -- cursor position */
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
    case 'J': /* ED -- erase in display */
        if (n == 2) {
            /* Clear entire visible area */
            for (int r = 0; r < tp->rows; r++)
                memset(grid_cell_abs(tp, r, 0), ' ', (unsigned)tp->cols);
            tp->cx = 0;
            tp->cy = 0;
        } else if (n == 0) {
            /* Clear from cursor to end */
            for (int c = tp->cx; c < tp->cols; c++)
                *grid_cell_abs(tp, tp->cy, c) = ' ';
            for (int r = tp->cy + 1; r < tp->rows; r++)
                memset(grid_cell_abs(tp, r, 0), ' ', (unsigned)tp->cols);
        }
        break;
    case 'K': /* EL -- erase in line */
        if (n == 0) {
            /* Clear from cursor to end of line */
            for (int c = tp->cx; c < tp->cols; c++)
                *grid_cell_abs(tp, tp->cy, c) = ' ';
        } else if (n == 2) {
            /* Clear entire line */
            memset(grid_cell_abs(tp, tp->cy, 0), ' ', (unsigned)tp->cols);
        }
        break;
    case 'm': /* SGR -- ignore (no color support yet) */
        break;
    default:
        break;
    }
}

void terminal_write(glyph_window_t *win, const char *data, int len)
{
    term_priv_t *tp = win->priv;

    /* Snap to bottom on new output */
    if (tp->scroll_offset > 0)
        tp->scroll_offset = 0;

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
            /* Inside CSI -- collect params and final byte */
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
                /* Final byte -- execute */
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
            *grid_cell_abs(tp, tp->cy, tp->cx) = (char)ch;
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

/* ---- Mouse callbacks for text selection ---- */

static void
term_mouse_down(glyph_window_t *win, int x, int y)
{
    term_priv_t *tp = win->priv;
    int ox, oy;
    get_padding(tp, &ox, &oy);

    int col = (x - ox) / tp->cell_w;
    int row = (y - oy) / tp->cell_h;

    /* Clamp to grid */
    if (col < 0) col = 0;
    if (col >= tp->cols) col = tp->cols - 1;
    if (row < 0) row = 0;
    if (row >= tp->rows) row = tp->rows - 1;

    tp->sel_active = 1;
    tp->sel_done = 0;
    tp->sel_start_col = col;
    tp->sel_start_row = row;
    tp->sel_end_col = col;
    tp->sel_end_row = row;

    glyph_window_mark_all_dirty(win);
}

static void
term_mouse_move(glyph_window_t *win, int x, int y)
{
    term_priv_t *tp = win->priv;
    if (!tp->sel_active || tp->sel_done)
        return;

    int ox, oy;
    get_padding(tp, &ox, &oy);

    int col = (x - ox) / tp->cell_w;
    int row = (y - oy) / tp->cell_h;

    if (col < 0) col = 0;
    if (col >= tp->cols) col = tp->cols - 1;
    if (row < 0) row = 0;
    if (row >= tp->rows) row = tp->rows - 1;

    if (col != tp->sel_end_col || row != tp->sel_end_row) {
        tp->sel_end_col = col;
        tp->sel_end_row = row;
        glyph_window_mark_all_dirty(win);
    }
}

static void
term_mouse_up(glyph_window_t *win, int x, int y)
{
    term_priv_t *tp = win->priv;
    if (!tp->sel_active)
        return;

    int ox, oy;
    get_padding(tp, &ox, &oy);

    int col = (x - ox) / tp->cell_w;
    int row = (y - oy) / tp->cell_h;

    if (col < 0) col = 0;
    if (col >= tp->cols) col = tp->cols - 1;
    if (row < 0) row = 0;
    if (row >= tp->rows) row = tp->rows - 1;

    tp->sel_end_col = col;
    tp->sel_end_row = row;

    /* If start == end, it was a click not a drag -- clear selection */
    if (tp->sel_start_col == tp->sel_end_col &&
        tp->sel_start_row == tp->sel_end_row) {
        tp->sel_active = 0;
    } else {
        tp->sel_done = 1;
    }

    glyph_window_mark_all_dirty(win);
}

static void
term_scroll_cb(glyph_window_t *win, int direction)
{
    term_priv_t *tp = win->priv;
    int max_offset = SCROLLBACK_LINES;

    if (direction > 0) {
        /* Scroll up into history */
        tp->scroll_offset += 3; /* 3 lines per scroll step */
        if (tp->scroll_offset > max_offset)
            tp->scroll_offset = max_offset;
    } else {
        /* Scroll down toward present */
        tp->scroll_offset -= 3;
        if (tp->scroll_offset < 0)
            tp->scroll_offset = 0;
    }

    glyph_window_mark_all_dirty(win);
}

/* ---- Public selection/clipboard helpers ---- */

int
terminal_has_selection(glyph_window_t *win)
{
    if (!win || !win->priv)
        return 0;
    term_priv_t *tp = win->priv;
    return tp->sel_active && tp->sel_done;
}

int
terminal_copy_selection(glyph_window_t *win, char *buf, int max)
{
    if (!win || !win->priv || max <= 0)
        return 0;
    term_priv_t *tp = win->priv;
    if (!tp->sel_active)
        return 0;

    int sr, sc, er, ec;
    sel_normalize(tp, &sr, &sc, &er, &ec);

    int pos = 0;
    for (int r = sr; r <= er && pos < max - 1; r++) {
        int c_start = (r == sr) ? sc : 0;
        int c_end = (r == er) ? ec : tp->cols - 1;

        /* Find last non-space in this row segment to strip trailing spaces */
        int last_nonspace = c_start - 1;
        for (int c = c_end; c >= c_start; c--) {
            char ch = *grid_cell(tp, r, c);
            if (ch != ' ') {
                last_nonspace = c;
                break;
            }
        }

        for (int c = c_start; c <= last_nonspace && pos < max - 1; c++) {
            buf[pos++] = *grid_cell(tp, r, c);
        }

        /* Add newline between lines (not after last line) */
        if (r < er && pos < max - 1)
            buf[pos++] = '\n';
    }

    buf[pos] = '\0';
    return pos;
}

void
terminal_clear_selection(glyph_window_t *win)
{
    if (!win || !win->priv)
        return;
    term_priv_t *tp = win->priv;
    tp->sel_active = 0;
    tp->sel_done = 0;
    glyph_window_mark_all_dirty(win);
}

void
terminal_scroll_back(glyph_window_t *win, int lines)
{
    if (!win || !win->priv)
        return;
    term_priv_t *tp = win->priv;
    tp->scroll_offset += lines;
    if (tp->scroll_offset > SCROLLBACK_LINES)
        tp->scroll_offset = SCROLLBACK_LINES;
    if (tp->scroll_offset < 0)
        tp->scroll_offset = 0;
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

    /* Terminal content starts at (4, 4) with small padding */
    int ox, oy;
    get_padding(tp, &ox, &oy);
    int cw = tp->cell_w;
    int ch = tp->cell_h;

    for (int r = 0; r < tp->rows; r++) {
        for (int c = 0; c < tp->cols; c++) {
            /* Draw selection highlight */
            if (cell_selected(tp, r, c)) {
                draw_blend_rect(s, ox + c * cw, oy + r * ch,
                                cw, ch, SEL_HIGHLIGHT, SEL_ALPHA);
            }

            char gc = *grid_cell(tp, r, c);
            if (gc != ' ') {
                if (g_font_mono)
                    font_draw_char(s, g_font_mono, 16,
                                   ox + c * cw, oy + r * ch,
                                   gc, C_TERM_FG);
                else
                    draw_char(s, ox + c * cw, oy + r * ch,
                              gc, C_TERM_FG, C_TERM_BG);
            }
        }
    }

    /* Cursor block (only when viewing bottom) */
    if (tp->master_fd >= 0 && tp->scroll_offset == 0)
        draw_fill_rect(s, ox + tp->cx * cw, oy + tp->cy * ch,
                       cw, ch, C_TERM_FG);

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
#define SYS_CAP_GRANT_EXEC 361
#define CAP_KIND_CAP_DELEGATE 13
#define CAP_KIND_CAP_QUERY    14
#define CAP_RIGHTS_READ       1

static int
spawn_shell(int slave_fd)
{
    char *argv[] = {"-stsh", NULL};  /* leading '-' = login shell */
    char *envp[] = {"PATH=/bin", "HOME=/root", "TERM=dumb", "USER=root", NULL};

    /* Pre-register caps so the spawned shell inherits them via exec_caps */
    syscall(SYS_CAP_GRANT_EXEC, (long)CAP_KIND_CAP_DELEGATE, (long)CAP_RIGHTS_READ);
    syscall(SYS_CAP_GRANT_EXEC, (long)CAP_KIND_CAP_QUERY, (long)CAP_RIGHTS_READ);

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

/* Allocate the ring-buffer grid and initialize a term_priv_t */
static term_priv_t *
term_priv_alloc(int cols, int rows, int cell_w, int cell_h,
                int pad_x, int pad_y, int is_dropdown)
{
    term_priv_t *tp = calloc(1, sizeof(*tp));
    if (!tp)
        return NULL;

    tp->master_fd = -1;
    tp->cols = cols;
    tp->rows = rows;
    tp->cell_w = cell_w;
    tp->cell_h = cell_h;
    tp->pad_x = pad_x;
    tp->pad_y = pad_y;
    tp->is_dropdown = is_dropdown;
    tp->total_rows = rows + SCROLLBACK_LINES;
    tp->scroll_top = 0;
    tp->scroll_offset = 0;

    tp->grid = calloc((unsigned)(tp->total_rows * cols), 1);
    if (!tp->grid) {
        free(tp);
        return NULL;
    }
    memset(tp->grid, ' ', (unsigned)(tp->total_rows * cols));

    return tp;
}

/* Wire up mouse callbacks on a window */
static void
term_wire_mouse(glyph_window_t *win)
{
    win->on_mouse_down = term_mouse_down;
    win->on_mouse_move = term_mouse_move;
    win->on_mouse_up = term_mouse_up;
    win->on_scroll = term_scroll_cb;
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
    int mono_w = g_font_mono ? font_text_width(g_font_mono, 16, "M") : FONT_W;
    int mono_h = g_font_mono ? font_height(g_font_mono, 16) : FONT_H;
    int cols = (dd_w - 2 * pad_x) / mono_w;
    int rows = (dd_h - 2 * pad_y) / mono_h;
    if (cols < 10) cols = 10;
    if (rows < 4) rows = 4;

    /* Allocate terminal private data with ring buffer */
    term_priv_t *tp = term_priv_alloc(cols, rows, mono_w, mono_h,
                                      pad_x, pad_y, 1);
    if (!tp) {
        *master_fd_out = -1;
        return NULL;
    }

    /* Create a glyph window */
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
    win->chromeless = 1;  /* no titlebar — pure terminal surface */
    win->visible = 0;  /* starts hidden */
    term_wire_mouse(win);

    /* Position: centered horizontally, below the taskbar (28px) */
    win->x = margin;
    win->y = 28;

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
    int mono_w = g_font_mono ? font_text_width(g_font_mono, 16, "M") : FONT_W;
    int mono_h = g_font_mono ? font_height(g_font_mono, 16) : FONT_H;
    int client_w = cols * mono_w;
    int client_h = rows * mono_h;
    glyph_window_t *win = glyph_window_create("Terminal", client_w, client_h);
    if (!win)
        return NULL;
    win->on_key = term_on_key;
    win->on_render = term_render_content;
    win->frosted = 1;

    /* Padding for normal terminal: border + titlebar */
    int pad_x = GLYPH_BORDER_WIDTH;
    int pad_y = GLYPH_BORDER_WIDTH + GLYPH_TITLEBAR_HEIGHT;

    /* Allocate terminal private data with ring buffer */
    term_priv_t *tp = term_priv_alloc(cols, rows, mono_w, mono_h,
                                      pad_x, pad_y, 0);
    if (!tp) {
        glyph_window_destroy(win);
        return NULL;
    }
    win->priv = tp;
    term_wire_mouse(win);

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
