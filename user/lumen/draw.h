/* draw.h — Framebuffer drawing primitives for Lumen compositor */
#ifndef LUMEN_DRAW_H
#define LUMEN_DRAW_H

#include <stdint.h>

#define FONT_W 10
#define FONT_H 20

typedef struct {
    uint32_t *buf;
    int w, h, pitch;  /* pitch in pixels */
} surface_t;

void draw_px(surface_t *s, int x, int y, uint32_t c);
void draw_fill_rect(surface_t *s, int x, int y, int w, int h, uint32_t c);
void draw_rect(surface_t *s, int x, int y, int w, int h, uint32_t c);
void draw_gradient_v(surface_t *s, int x, int y, int w, int h, uint32_t c1, uint32_t c2);
void draw_char(surface_t *s, int x, int y, char ch, uint32_t fg, uint32_t bg);
void draw_text(surface_t *s, int x, int y, const char *str, uint32_t fg, uint32_t bg);
void draw_text_t(surface_t *s, int x, int y, const char *str, uint32_t fg);
void draw_blit(surface_t *dst, int dx, int dy, const uint32_t *src, int sw, int sh);

#define C_BG1       0x001B2838
#define C_BG2       0x000D1B2A
#define C_BAR       0x001A1A2E
#define C_BAR_T     0x00C0C0D0
#define C_WIN       0x00FFFFFF
#define C_BORDER    0x00404060
#define C_TITLE1    0x003468A0
#define C_TITLE2    0x00205078
#define C_UTITLE1   0x00606070
#define C_UTITLE2   0x00404050
#define C_TITLE_T   0x00FFFFFF
#define C_TEXT      0x00202030
#define C_RED       0x00E04040
#define C_GREEN     0x0040B040
#define C_YELLOW    0x00E0C040
#define C_BTN       0x003070A0
#define C_BTN_T     0x00FFFFFF
#define C_ACCENT    0x004488CC
#define C_SUBTLE    0x00808090
#define C_SHADOW    0x00080810
#define C_TERM_FG   0x0040FF40
#define C_TERM_BG   0x000A0A14

#endif
