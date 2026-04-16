/* lumen_client.h — Client API for connecting external processes to Lumen */
#ifndef LUMEN_CLIENT_H
#define LUMEN_CLIENT_H

#include <stdint.h>
#include "lumen_proto.h"

typedef struct {
    int      fd;      /* connection socket */
    uint32_t id;      /* window_id assigned by Lumen */
    int      memfd;   /* shared pixel buffer fd */
    void    *shared;  /* MAP_SHARED — Lumen reads this */
    void    *backbuf; /* malloc'd — caller renders here */
    int      w, h;    /* client area dimensions in pixels */
    int      stride;  /* stride in pixels (== w in v1) */
    int      x, y;    /* screen position Lumen placed the window at */
} lumen_window_t;

typedef struct {
    uint32_t type;       /* LUMEN_EV_* opcode */
    uint32_t window_id;
    union {
        struct { uint32_t keycode, modifiers; uint8_t pressed; } key;
        struct { int32_t x, y; uint8_t buttons, evtype; }        mouse;
        struct { uint8_t focused; }                               focus;
        struct { uint32_t new_w, new_h; }                        resized;
    };
} lumen_event_t;

int lumen_connect(void);
lumen_window_t *lumen_window_create(int fd, const char *title, int w, int h);
lumen_window_t *lumen_panel_create(int fd, int w, int h);
void lumen_window_present(lumen_window_t *win);
int lumen_poll_event(int fd, lumen_event_t *ev);
int lumen_wait_event(int fd, lumen_event_t *ev, int timeout_ms);
void lumen_window_destroy(lumen_window_t *win);
int lumen_invoke(int fd, const char *name);

#endif /* LUMEN_CLIENT_H */
