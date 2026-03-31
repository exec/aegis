/* taskbar.h — top bar for Aegis desktop shell */
#ifndef CITADEL_TASKBAR_H
#define CITADEL_TASKBAR_H

#include <glyph.h>

void topbar_draw(surface_t *s, int screen_w, const char *clock_str);
int topbar_hit_aegis(int mx, int my, int screen_w);

#endif
