/* lumen_proto.h — Lumen external window protocol wire structs.
 * Included by both the client library and the Lumen server. */
#ifndef LUMEN_PROTO_H
#define LUMEN_PROTO_H

#include <stdint.h>

#define LUMEN_MAGIC     0x4C4D454Eu  /* "LMEN" */
#define LUMEN_VERSION   1u

/* ── Handshake (no lumen_msg_hdr_t wrapper — sent raw) ──────────────── */

typedef struct {
    uint32_t magic;    /* LUMEN_MAGIC */
    uint32_t version;  /* LUMEN_VERSION */
} lumen_hello_t;

typedef struct {
    uint32_t magic;    /* LUMEN_MAGIC */
    uint32_t version;  /* echoed */
    uint32_t status;   /* 0=OK, 1=version unsupported, 2=server full */
} lumen_hello_reply_t;

/* ── Common framed message header ───────────────────────────────────── */

typedef struct {
    uint32_t op;   /* opcode */
    uint32_t len;  /* bytes of payload following this header */
} lumen_msg_hdr_t;

/* ── Client → server opcodes ────────────────────────────────────────── */

#define LUMEN_OP_CREATE_WINDOW   1u
#define LUMEN_OP_DAMAGE          2u
#define LUMEN_OP_SET_TITLE       3u
#define LUMEN_OP_DESTROY_WINDOW  4u
#define LUMEN_OP_CREATE_PANEL    5u  /* chromeless, bottom-anchored, no focus */
#define LUMEN_OP_INVOKE          6u  /* ask server to run a built-in by name */

typedef struct {
    uint16_t width;
    uint16_t height;
    char     title[64];  /* null-terminated */
} lumen_create_window_t;

/* Panel: no chrome, no focus, server positions at bottom-center.
 * Reply uses lumen_window_created_t (same as CREATE_WINDOW) + memfd. */
typedef struct {
    uint16_t width;
    uint16_t height;
} lumen_create_panel_t;

typedef struct {
    char name[32];   /* null-terminated, e.g. "terminal", "widgets" */
} lumen_invoke_t;

typedef struct {
    uint32_t window_id;
} lumen_damage_t;

typedef struct {
    uint32_t window_id;
    char     title[64];
} lumen_set_title_t;

typedef struct {
    uint32_t window_id;
} lumen_destroy_window_t;

/* ── Server → client (CREATE_WINDOW reply) ──────────────────────────── */
/* Sent as framed message (hdr.op=0) via sendmsg with SCM_RIGHTS memfd  */

typedef struct {
    uint32_t status;     /* 0=OK, nonzero=errno */
    uint32_t window_id;
    uint32_t width;      /* actual width Lumen assigned */
    uint32_t height;     /* actual height Lumen assigned */
    int32_t  x, y;       /* screen position Lumen placed the window at */
} lumen_window_created_t;

/* ── Server → client event opcodes ─────────────────────────────────── */

#define LUMEN_EV_KEY           0x10u
#define LUMEN_EV_MOUSE         0x11u
#define LUMEN_EV_CLOSE_REQUEST 0x12u
#define LUMEN_EV_FOCUS         0x13u
#define LUMEN_EV_RESIZED       0x14u  /* v1: defined but never sent */

/* LUMEN_EV_MOUSE evtype values */
#define LUMEN_MOUSE_MOVE  0u
#define LUMEN_MOUSE_DOWN  1u
#define LUMEN_MOUSE_UP    2u

typedef struct {
    uint32_t window_id;
    uint32_t keycode;    /* (uint8_t)char from keyboard ISR */
    uint32_t modifiers;  /* reserved, 0 in v1 */
    uint8_t  pressed;    /* 1=down */
    uint8_t  _pad[3];
} lumen_key_event_t;

typedef struct {
    uint32_t window_id;
    int32_t  x, y;       /* client-area-relative (after subtracting chrome) */
    uint8_t  buttons;    /* bitmask: bit0=left, bit1=right, bit2=middle */
    uint8_t  evtype;     /* LUMEN_MOUSE_* */
    uint8_t  _pad[2];
} lumen_mouse_event_t;

typedef struct {
    uint32_t window_id;
} lumen_close_request_t;

typedef struct {
    uint32_t window_id;
    uint8_t  focused;
    uint8_t  _pad[3];
} lumen_focus_event_t;

typedef struct {
    uint32_t window_id;
    uint32_t new_width;
    uint32_t new_height;
} lumen_resized_event_t;

#endif /* LUMEN_PROTO_H */
