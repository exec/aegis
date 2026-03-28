/* taskbar.c — Desktop taskbar for Lumen compositor */
#include "compositor.h"
#include "draw.h"

void
taskbar_draw(surface_t *s, int screen_w, int screen_h)
{
    int tb_y = screen_h - TASKBAR_HEIGHT;
    draw_fill_rect(s, 0, tb_y, screen_w, TASKBAR_HEIGHT, C_BAR);
    draw_fill_rect(s, 0, tb_y, screen_w, 1, C_ACCENT);
    draw_fill_rect(s, 6, tb_y + 6, 90, 24, C_BTN);
    draw_rect(s, 6, tb_y + 6, 90, 24, C_ACCENT);
    draw_text(s, 16, tb_y + 8, "  Aegis", C_BTN_T, C_BTN);
    draw_text(s, screen_w - 100, tb_y + 8, "12:00 AM", C_BAR_T, C_BAR);
}
