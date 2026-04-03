/* dock.h -- macOS-style dock for Lumen compositor */
#ifndef LUMEN_DOCK_H
#define LUMEN_DOCK_H

#include <glyph.h>

#define DOCK_ICON_SIZE  48
#define DOCK_ICON_GAP   12
#define DOCK_PADDING_X  24
#define DOCK_PADDING_Y  16
#define DOCK_HEIGHT     (DOCK_ICON_SIZE + DOCK_PADDING_Y * 2)
#define DOCK_CORNER_R   16
#define DOCK_BOTTOM_MARGIN 10

#define DOCK_BG         0x002A2A3E
#define DOCK_HOVER_BG   0x00FFFFFF
#define DOCK_HOVER_ALPHA 40

/* Frosted glass parameters */
#define DOCK_BLUR_RADIUS   10
#define DOCK_GLASS_TINT    0x001A1A2E
#define DOCK_GLASS_ALPHA   160

/* Dock item IDs */
#define DOCK_ITEM_SETTINGS  0
#define DOCK_ITEM_FILES     1
#define DOCK_ITEM_TERMINAL  2
#define DOCK_ITEM_WIDGETS   3
#define DOCK_ITEM_COUNT     4

void dock_init(int screen_w, int screen_h);
void dock_draw(surface_t *fb, int screen_w, int screen_h);
int dock_hit_test(int mx, int my);   /* returns item index or -1 */
void dock_set_hover(int item);        /* -1 for no hover */

#endif
