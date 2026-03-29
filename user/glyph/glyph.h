/* glyph.h -- Glyph widget toolkit public API
 *
 * Retained-mode widget tree with dirty-rect propagation.
 * All widget structs embed glyph_widget_t as their first member.
 */
#ifndef GLYPH_H
#define GLYPH_H

#include "draw.h"
#include <stdint.h>

/* ---- Geometry ---- */

typedef struct {
    int x, y, w, h;
} glyph_rect_t;

glyph_rect_t glyph_rect_union(glyph_rect_t a, glyph_rect_t b);
int glyph_rect_empty(glyph_rect_t r);
int glyph_rect_intersects(glyph_rect_t a, glyph_rect_t b);

/* ---- Widget type enum ---- */

typedef enum {
    GLYPH_WIDGET_LABEL,
    GLYPH_WIDGET_BUTTON,
    GLYPH_WIDGET_TEXTFIELD,
    GLYPH_WIDGET_CHECKBOX,
    GLYPH_WIDGET_LISTVIEW,
    GLYPH_WIDGET_SCROLLBAR,
    GLYPH_WIDGET_IMAGE,
    GLYPH_WIDGET_PROGRESS,
    GLYPH_WIDGET_MENUBAR,
    GLYPH_WIDGET_TABS,
    GLYPH_WIDGET_HBOX,
    GLYPH_WIDGET_VBOX,
} glyph_widget_type_t;

/* ---- Forward declarations ---- */

typedef struct glyph_widget glyph_widget_t;
typedef struct glyph_window glyph_window_t;

/* ---- Base widget ---- */

#define GLYPH_MAX_CHILDREN 32

struct glyph_widget {
    glyph_widget_type_t type;
    glyph_widget_t *parent;
    glyph_widget_t *children[GLYPH_MAX_CHILDREN];
    int nchildren;

    /* Bounds relative to parent's client area (or window client area for root) */
    int x, y, w, h;

    /* Preferred size for layout */
    int pref_w, pref_h;

    /* Flags */
    int dirty;
    int visible;
    int focusable;

    /* Callbacks -- set by concrete widget types */
    void (*draw_fn)(glyph_widget_t *self, surface_t *surf, int ox, int oy);
    void (*on_mouse)(glyph_widget_t *self, int btn, int local_x, int local_y);
    void (*on_key)(glyph_widget_t *self, char key);
    void (*on_destroy)(glyph_widget_t *self);

    /* Owning window -- set when added to a window's widget tree */
    glyph_window_t *window;
};

/* ---- Widget tree operations ---- */

void glyph_widget_init(glyph_widget_t *w, glyph_widget_type_t type);
void glyph_widget_add_child(glyph_widget_t *parent, glyph_widget_t *child);
void glyph_widget_remove_child(glyph_widget_t *parent, glyph_widget_t *child);
void glyph_widget_mark_dirty(glyph_widget_t *w);
glyph_widget_t *glyph_widget_hit_test(glyph_widget_t *root, int x, int y);
glyph_widget_t *glyph_widget_focus_next(glyph_widget_t *root, glyph_widget_t *current);
void glyph_widget_draw_tree(glyph_widget_t *root, surface_t *surf, int ox, int oy);
void glyph_widget_destroy_tree(glyph_widget_t *root);
void glyph_widget_set_window(glyph_widget_t *root, glyph_window_t *win);

/* ---- Window ---- */

#define GLYPH_TITLEBAR_HEIGHT 30
#define GLYPH_BORDER_WIDTH    1
#define GLYPH_SHADOW_OFFSET   4

struct glyph_window {
    /* Position on screen (top-left of border) */
    int x, y;

    /* Client area dimensions */
    int client_w, client_h;

    /* Total surface including chrome */
    int surf_w, surf_h;
    surface_t surface;

    /* Title */
    char title[64];

    /* Root widget for client area */
    glyph_widget_t *root;

    /* Focus */
    glyph_widget_t *focused;

    /* Dirty tracking */
    glyph_rect_t dirty_rect;
    int has_dirty;

    /* Flags */
    int visible;
    int closeable;
    int focused_window; /* 1 if this is the compositor's focused window */

    /* Callbacks for compositor integration */
    void (*on_key)(glyph_window_t *self, char key);
    void (*on_close)(glyph_window_t *self);
    void (*on_render)(glyph_window_t *self);  /* custom client area render (after chrome) */
    void *priv;
};

glyph_window_t *glyph_window_create(const char *title, int client_w, int client_h);
void glyph_window_destroy(glyph_window_t *win);
void glyph_window_render(glyph_window_t *win);
void glyph_window_dispatch_mouse(glyph_window_t *win, int btn, int x, int y);
void glyph_window_dispatch_key(glyph_window_t *win, char key);
int glyph_window_get_dirty_rect(glyph_window_t *win, glyph_rect_t *out);
void glyph_window_set_focus(glyph_window_t *win, glyph_widget_t *widget);
void glyph_window_set_content(glyph_window_t *win, glyph_widget_t *root);
void glyph_window_mark_dirty_rect(glyph_window_t *win, glyph_rect_t r);
void glyph_window_mark_all_dirty(glyph_window_t *win);

/* ---- Layout containers ---- */

typedef struct {
    glyph_widget_t base;
    int padding;
    int spacing;
} glyph_box_t;

glyph_box_t *glyph_hbox_create(int padding, int spacing);
glyph_box_t *glyph_vbox_create(int padding, int spacing);
void glyph_box_layout(glyph_box_t *box);

/* ---- Label ---- */

typedef struct {
    glyph_widget_t base;
    char text[128];
    uint32_t color;
    uint32_t bg_color;
    int transparent; /* 1 = use draw_text_t (no bg fill) */
} glyph_label_t;

glyph_label_t *glyph_label_create(const char *text, uint32_t color);
void glyph_label_set_text(glyph_label_t *label, const char *text);
void glyph_label_set_bg(glyph_label_t *label, uint32_t bg);
void glyph_label_set_transparent(glyph_label_t *label, int transparent);

/* ---- Button ---- */

typedef enum {
    GLYPH_BTN_NORMAL,
    GLYPH_BTN_HOVER,
    GLYPH_BTN_PRESSED,
    GLYPH_BTN_DISABLED,
} glyph_btn_state_t;

typedef struct {
    glyph_widget_t base;
    char text[64];
    glyph_btn_state_t state;
    void (*on_click)(glyph_widget_t *self);
} glyph_button_t;

glyph_button_t *glyph_button_create(const char *text, void (*on_click)(glyph_widget_t *));
void glyph_button_set_text(glyph_button_t *btn, const char *text);
void glyph_button_set_state(glyph_button_t *btn, glyph_btn_state_t state);

/* ---- Text field ---- */

typedef struct {
    glyph_widget_t base;
    char buf[256];
    int len;
    int cursor_pos;
    int width_chars;
    void (*on_change)(glyph_widget_t *self, const char *text);
} glyph_textfield_t;

glyph_textfield_t *glyph_textfield_create(int width_chars, void (*on_change)(glyph_widget_t *, const char *));
const char *glyph_textfield_get_text(glyph_textfield_t *tf);
void glyph_textfield_set_text(glyph_textfield_t *tf, const char *text);

/* ---- Checkbox ---- */

typedef struct {
    glyph_widget_t base;
    char label[64];
    int checked;
    void (*on_change)(glyph_widget_t *self, int checked);
} glyph_checkbox_t;

glyph_checkbox_t *glyph_checkbox_create(const char *label, void (*on_change)(glyph_widget_t *, int));

/* ---- Progress bar ---- */

typedef struct {
    glyph_widget_t base;
    int value; /* 0-100 */
} glyph_progress_t;

glyph_progress_t *glyph_progress_create(int width);
void glyph_progress_set_value(glyph_progress_t *bar, int value);

/* ---- Image ---- */

typedef struct {
    glyph_widget_t base;
    uint32_t *pixels;
    int img_w, img_h;
} glyph_image_t;

glyph_image_t *glyph_image_create(uint32_t *pixels, int w, int h);

/* ---- Scroll bar ---- */

typedef struct {
    glyph_widget_t base;
    int min_val, max_val;
    int value;
    int page_size;
    int thumb_dragging;
    int drag_offset;
    void (*on_scroll)(glyph_widget_t *self, int value);
} glyph_scrollbar_t;

glyph_scrollbar_t *glyph_scrollbar_create(int height, void (*on_scroll)(glyph_widget_t *, int));
void glyph_scrollbar_set_range(glyph_scrollbar_t *sb, int max_val, int page_size);

/* ---- List view ---- */

#define GLYPH_LISTVIEW_MAX_ITEMS 256

typedef struct {
    glyph_widget_t base;
    const char *items[GLYPH_LISTVIEW_MAX_ITEMS];
    int count;
    int selected;
    int scroll_offset;
    int visible_rows;
    glyph_scrollbar_t *scrollbar;
    void (*on_select)(glyph_widget_t *self, int index);
} glyph_listview_t;

glyph_listview_t *glyph_listview_create(int width, int visible_rows, void (*on_select)(glyph_widget_t *, int));
void glyph_listview_set_items(glyph_listview_t *lv, const char **items, int count);

/* ---- Menu bar ---- */

#define GLYPH_MENU_MAX_MENUS 8
#define GLYPH_MENU_MAX_ITEMS 16

typedef struct {
    char label[32];
    char items[GLYPH_MENU_MAX_ITEMS][32];
    int count;
} glyph_menu_def_t;

typedef struct {
    glyph_widget_t base;
    glyph_menu_def_t menus[GLYPH_MENU_MAX_MENUS];
    int nmenu;
    int open_idx;   /* -1 = all closed */
    void (*on_select)(glyph_widget_t *self, int menu_idx, int item_idx);
} glyph_menubar_t;

glyph_menubar_t *glyph_menubar_create(void (*on_select)(glyph_widget_t *, int, int));
void glyph_menubar_add_menu(glyph_menubar_t *mb, const char *label, const char **items, int count);

/* ---- Tabs ---- */

#define GLYPH_TABS_MAX 8

typedef struct {
    glyph_widget_t base;
    char labels[GLYPH_TABS_MAX][32];
    glyph_widget_t *panels[GLYPH_TABS_MAX];
    int ntabs;
    int active;
    void (*on_change)(glyph_widget_t *self, int index);
} glyph_tabs_t;

glyph_tabs_t *glyph_tabs_create(void (*on_change)(glyph_widget_t *, int));
void glyph_tabs_add(glyph_tabs_t *tabs, const char *label, glyph_widget_t *panel);

#endif /* GLYPH_H */
