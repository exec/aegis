/* terminal.h -- PTY-based terminal emulator window for Lumen */
#ifndef LUMEN_TERMINAL_H
#define LUMEN_TERMINAL_H

#include <glyph.h>

glyph_window_t *terminal_create(int cols, int rows, int *master_fd_out);
glyph_window_t *terminal_create_dropdown(int screen_w, int screen_h,
                                         int *master_fd_out);
void terminal_write(glyph_window_t *win, const char *data, int len);

/* Selection / clipboard helpers */
int terminal_has_selection(glyph_window_t *win);
int terminal_copy_selection(glyph_window_t *win, char *buf, int max);
void terminal_clear_selection(glyph_window_t *win);
void terminal_scroll_back(glyph_window_t *win, int lines);

#endif
