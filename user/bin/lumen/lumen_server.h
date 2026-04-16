/* lumen_server.h — Lumen external window server interface */
#ifndef LUMEN_SERVER_H
#define LUMEN_SERVER_H

#include "compositor.h"

int lumen_server_init(void);
int lumen_server_tick(compositor_t *comp, int listen_fd);

/* Notify a proxy window of focus change. win may be NULL or non-proxy (no-op). */
void lumen_proxy_notify_focus(glyph_window_t *win, int focused);

/* Register a handler for LUMEN_OP_INVOKE messages.
 * The handler receives the compositor and the requested name.
 * Lumen's main.c registers a dispatcher that maps "terminal" → spawn_terminal,
 * "widgets" → widget_test_create, etc. */
typedef void (*lumen_invoke_fn)(compositor_t *comp, const char *name);
void lumen_server_set_invoke_handler(lumen_invoke_fn fn);

#endif /* LUMEN_SERVER_H */
