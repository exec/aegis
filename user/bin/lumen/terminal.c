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

/* ANSI 8-color palette (standard + bright) */
static const uint32_t ansi_colors[16] = {
    0x000000, 0xCC0000, 0x00CC00, 0xCCAA00,  /* black, red, green, yellow */
    0x4488CC, 0xCC00CC, 0x00CCCC, 0xCCCCCC,  /* blue, magenta, cyan, white */
    0x555555, 0xFF5555, 0x55FF55, 0xFFFF55,  /* bright variants */
    0x5599FF, 0xFF55FF, 0x55FFFF, 0xFFFFFF,
};

typedef struct {
    char ch;
    uint8_t fg;  /* index into ansi_colors, 0xFF = default */
    uint8_t bg;  /* index into ansi_colors, 0xFF = default */
} term_cell_t;

typedef struct {
    int master_fd;  /* -1 if PTY failed */
    int cols, rows;
    int cx, cy;     /* cursor position (relative to visible area bottom) */
    int cell_w, cell_h; /* character cell size (TTF or bitmap fallback) */
    term_cell_t *grid;  /* ring buffer: total_rows * cols */
    uint8_t cur_fg, cur_bg;  /* current SGR attributes (0xFF = default) */
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
static inline term_cell_t *
grid_cell(term_priv_t *tp, int vis_row, int col)
{
    int ring_row = (tp->scroll_top - tp->scroll_offset + vis_row) % tp->total_rows;
    if (ring_row < 0)
        ring_row += tp->total_rows;
    return &tp->grid[ring_row * tp->cols + col];
}

/* Access a cell at the cursor's absolute position (ignoring scroll_offset).
 * Used by the output path which always writes at the bottom. */
static inline term_cell_t *
grid_cell_abs(term_priv_t *tp, int vis_row, int col)
{
    int ring_row = (tp->scroll_top + vis_row) % tp->total_rows;
    if (ring_row < 0)
        ring_row += tp->total_rows;
    return &tp->grid[ring_row * tp->cols + col];
}

static void term_scroll(term_priv_t *tp)
{
    tp->scroll_top = (tp->scroll_top + 1) % tp->total_rows;
    int new_bottom = (tp->scroll_top + tp->rows - 1) % tp->total_rows;
    term_cell_t *row = &tp->grid[new_bottom * tp->cols];
    for (int i = 0; i < tp->cols; i++)
        row[i] = (term_cell_t){' ', 0xFF, 0xFF};
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

/* Check if a cell (r, c) is within a pre-normalized selection range */
static inline int
cell_in_sel(int r, int c, int sr, int sc, int er, int ec)
{
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

/* Shared terminal content renderer for both normal and dropdown terminals */
static void render_term_grid(glyph_window_t *win)
{
    term_priv_t *tp = win->priv;
    surface_t *s = &win->surface;
    int ox, oy;
    get_padding(tp, &ox, &oy);
    int cw = tp->cell_w;
    int ch = tp->cell_h;

    /* Clear client area */
    if (tp->is_dropdown)
        draw_fill_rect(s, 0, 0, win->surf_w, win->surf_h, C_TERM_BG);
    else
        draw_fill_rect(s, ox, oy, win->client_w, win->client_h, C_TERM_BG);

    /* Pre-normalize selection once (not per-cell) */
    int has_sel = tp->sel_active;
    int sr = 0, sc = 0, er = 0, ec = 0;
    if (has_sel)
        sel_normalize(tp, &sr, &sc, &er, &ec);

    for (int r = 0; r < tp->rows; r++) {
        /* Quick check: skip entirely blank, unselected rows */
        term_cell_t *row_start = grid_cell(tp, r, 0);
        int row_has_content = 0;
        int row_has_sel = has_sel && r >= sr && r <= er;

        if (!row_has_sel) {
            for (int c = 0; c < tp->cols; c++) {
                if (row_start[c].ch != ' ' || row_start[c].bg != 0xFF) {
                    row_has_content = 1;
                    break;
                }
            }
            if (!row_has_content)
                continue;
        }

        for (int c = 0; c < tp->cols; c++) {
            term_cell_t *cell = grid_cell(tp, r, c);
            int px = ox + c * cw;
            int py = oy + r * ch;

            /* Draw cell background if non-default */
            if (cell->bg != 0xFF && cell->bg < 16)
                draw_fill_rect(s, px, py, cw, ch, ansi_colors[cell->bg]);

            if (row_has_sel && cell_in_sel(r, c, sr, sc, er, ec))
                draw_blend_rect(s, px, py, cw, ch, SEL_HIGHLIGHT, SEL_ALPHA);

            if (cell->ch != ' ') {
                uint32_t fg = (cell->fg != 0xFF && cell->fg < 16)
                              ? ansi_colors[cell->fg] : C_TERM_FG;
                if (g_font_mono)
                    font_draw_char(s, g_font_mono, 16, px, py, cell->ch, fg);
                else
                    draw_char(s, px, py, cell->ch, fg, C_TERM_BG);
            }
        }
    }

    /* Cursor block (only when viewing bottom) */
    if (tp->master_fd >= 0 && tp->scroll_offset == 0)
        draw_fill_rect(s, ox + tp->cx * cw, oy + tp->cy * ch,
                       cw, ch, C_TERM_FG);
}

static void term_render_content(glyph_window_t *win)
{
    render_term_grid(win);
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
            term_cell_t *cell = grid_cell_abs(tp, tp->cy, tp->cx);
            cell->ch = (char)ch;
            cell->fg = 0xFF;
            cell->bg = 0xFF;
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
        {
            term_cell_t blank = {' ', 0xFF, 0xFF};
            if (n == 2) {
                for (int r = 0; r < tp->rows; r++)
                    for (int c2 = 0; c2 < tp->cols; c2++)
                        *grid_cell_abs(tp, r, c2) = blank;
                tp->cx = 0;
                tp->cy = 0;
            } else if (n == 0) {
                for (int c2 = tp->cx; c2 < tp->cols; c2++)
                    *grid_cell_abs(tp, tp->cy, c2) = blank;
                for (int r = tp->cy + 1; r < tp->rows; r++)
                    for (int c2 = 0; c2 < tp->cols; c2++)
                        *grid_cell_abs(tp, r, c2) = blank;
            }
        }
        break;
    case 'K': /* EL -- erase in line */
        {
            term_cell_t blank = {' ', 0xFF, 0xFF};
            if (n == 0) {
                for (int c2 = tp->cx; c2 < tp->cols; c2++)
                    *grid_cell_abs(tp, tp->cy, c2) = blank;
            } else if (n == 2) {
                for (int c2 = 0; c2 < tp->cols; c2++)
                    *grid_cell_abs(tp, tp->cy, c2) = blank;
            }
        }
        break;
    case 'm': /* SGR -- set graphic rendition (colors) */
        if (tp->esc_nparam == 0) {
            /* ESC[m = reset */
            tp->cur_fg = 0xFF;
            tp->cur_bg = 0xFF;
        }
        for (int pi = 0; pi < tp->esc_nparam; pi++) {
            int p = tp->esc_params[pi];
            if (p == 0) { tp->cur_fg = 0xFF; tp->cur_bg = 0xFF; }
            else if (p >= 30 && p <= 37) tp->cur_fg = (uint8_t)(p - 30);
            else if (p == 39) tp->cur_fg = 0xFF;
            else if (p >= 40 && p <= 47) tp->cur_bg = (uint8_t)(p - 40);
            else if (p == 49) tp->cur_bg = 0xFF;
            else if (p >= 90 && p <= 97) tp->cur_fg = (uint8_t)(p - 90 + 8);
            else if (p >= 100 && p <= 107) tp->cur_bg = (uint8_t)(p - 100 + 8);
            else if (p == 1) { /* bold → bright variant */
                if (tp->cur_fg != 0xFF && tp->cur_fg < 8) tp->cur_fg += 8;
            }
        }
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
            term_cell_t *cell = grid_cell_abs(tp, tp->cy, tp->cx);
            cell->ch = (char)ch;
            cell->fg = tp->cur_fg;
            cell->bg = tp->cur_bg;
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
            if (grid_cell(tp, r, c)->ch != ' ') {
                last_nonspace = c;
                break;
            }
        }

        for (int c = c_start; c <= last_nonspace && pos < max - 1; c++) {
            buf[pos++] = grid_cell(tp, r, c)->ch;
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

static void
dropdown_render_content(glyph_window_t *win)
{
    render_term_grid(win);

    /* Round the bottom corners by painting the background color
     * over pixels that fall outside the corner arcs. */
    surface_t *s = &win->surface;
    int r = DROPDOWN_CORNER_R;
    int sw = win->surf_w;
    int sh = win->surf_h;
    for (int y = sh - r; y < sh; y++) {
        int dy = y - (sh - r - 1);
        /* Left corner: fill [0, cutoff) with bg */
        int cutoff_l = r;
        for (int px = 0; px < r; px++) {
            int dx = r - px;
            if (dx * dx + dy * dy <= r * r) { cutoff_l = px; break; }
        }
        if (cutoff_l > 0)
            draw_fill_rect(s, 0, y, cutoff_l, 1, C_BG1);
        /* Right corner: fill [sw - cutoff, sw) with bg */
        int cutoff_r = r;
        for (int px = sw - 1; px >= sw - r; px--) {
            int dx = px - (sw - r - 1);
            if (dx * dx + dy * dy <= r * r) { cutoff_r = sw - 1 - px; break; }
        }
        if (cutoff_r > 0)
            draw_fill_rect(s, sw - cutoff_r, y, cutoff_r, 1, C_BG1);
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

    tp->cur_fg = 0xFF;
    tp->cur_bg = 0xFF;
    tp->grid = calloc((unsigned)(tp->total_rows * cols), sizeof(term_cell_t));
    if (!tp->grid) {
        free(tp);
        return NULL;
    }
    for (int i = 0; i < tp->total_rows * cols; i++)
        tp->grid[i] = (term_cell_t){' ', 0xFF, 0xFF};

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
