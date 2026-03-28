/* terminal.h -- PTY-based terminal emulator window for Lumen */
#ifndef LUMEN_TERMINAL_H
#define LUMEN_TERMINAL_H

#include "compositor.h"

window_t *terminal_create(int cols, int rows, int *master_fd_out);
void terminal_write(window_t *win, const char *data, int len);

#endif
