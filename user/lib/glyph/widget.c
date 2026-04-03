/* widget.c -- Base widget tree operations for Glyph toolkit */
#include "glyph.h"
#include <stdlib.h>
#include <string.h>

/* ---- Geometry helpers ---- */

glyph_rect_t
glyph_rect_union(glyph_rect_t a, glyph_rect_t b)
{
    if (glyph_rect_empty(a)) return b;
    if (glyph_rect_empty(b)) return a;

    int x1 = a.x < b.x ? a.x : b.x;
    int y1 = a.y < b.y ? a.y : b.y;
    int x2a = a.x + a.w;
    int x2b = b.x + b.w;
    int y2a = a.y + a.h;
    int y2b = b.y + b.h;
    int x2 = x2a > x2b ? x2a : x2b;
    int y2 = y2a > y2b ? y2a : y2b;

    glyph_rect_t r;
    r.x = x1;
    r.y = y1;
    r.w = x2 - x1;
    r.h = y2 - y1;
    return r;
}

int
glyph_rect_empty(glyph_rect_t r)
{
    return r.w <= 0 || r.h <= 0;
}

int
glyph_rect_intersects(glyph_rect_t a, glyph_rect_t b)
{
    return a.x < b.x + b.w && a.x + a.w > b.x &&
           a.y < b.y + b.h && a.y + a.h > b.y;
}

/* ---- Widget init ---- */

void
glyph_widget_init(glyph_widget_t *w, glyph_widget_type_t type)
{
    memset(w, 0, sizeof(*w));
    w->type = type;
    w->visible = 1;
    w->dirty = 1;
}

/* ---- Tree operations ---- */

void
glyph_widget_add_child(glyph_widget_t *parent, glyph_widget_t *child)
{
    if (!parent || !child)
        return;
    if (parent->nchildren >= GLYPH_MAX_CHILDREN)
        return;
    child->parent = parent;
    child->window = parent->window;
    parent->children[parent->nchildren++] = child;

    /* Auto-layout box containers when children are added */
    if (parent->type == GLYPH_WIDGET_HBOX || parent->type == GLYPH_WIDGET_VBOX)
        glyph_box_layout((glyph_box_t *)parent);

    glyph_widget_mark_dirty(parent);
}

void
glyph_widget_remove_child(glyph_widget_t *parent, glyph_widget_t *child)
{
    if (!parent || !child)
        return;
    int idx = -1;
    for (int i = 0; i < parent->nchildren; i++) {
        if (parent->children[i] == child) {
            idx = i;
            break;
        }
    }
    if (idx < 0)
        return;

    for (int i = idx; i < parent->nchildren - 1; i++)
        parent->children[i] = parent->children[i + 1];
    parent->nchildren--;
    child->parent = NULL;
    child->window = NULL;
    glyph_widget_mark_dirty(parent);
}

void
glyph_widget_mark_dirty(glyph_widget_t *w)
{
    if (!w)
        return;
    w->dirty = 1;

    /* Propagate dirty rect up to the window */
    if (w->window) {
        /* Compute widget's absolute rect relative to window client area */
        int ax = 0, ay = 0;
        glyph_widget_t *p = w;
        while (p) {
            ax += p->x;
            ay += p->y;
            p = p->parent;
        }
        glyph_rect_t r = { ax, ay, w->w, w->h };
        glyph_window_mark_dirty_rect(w->window, r);
    }
}

/* ---- Hit test (depth-first, deepest match wins) ---- */

static glyph_widget_t *
hit_test_recursive(glyph_widget_t *w, int x, int y)
{
    if (!w || !w->visible)
        return NULL;

    /* Check if point is within this widget's bounds */
    if (x < w->x || x >= w->x + w->w || y < w->y || y >= w->y + w->h)
        return NULL;

    /* Check children in reverse order (topmost first) */
    int local_x = x - w->x;
    int local_y = y - w->y;
    for (int i = w->nchildren - 1; i >= 0; i--) {
        glyph_widget_t *hit = hit_test_recursive(w->children[i], local_x, local_y);
        if (hit)
            return hit;
    }

    /* No child matched, return this widget */
    return w;
}

glyph_widget_t *
glyph_widget_hit_test(glyph_widget_t *root, int x, int y)
{
    return hit_test_recursive(root, x, y);
}

/* ---- Focus cycling ---- */

static glyph_widget_t *
find_next_focusable(glyph_widget_t *w, int *found_current, glyph_widget_t *current)
{
    if (!w || !w->visible)
        return NULL;

    if (w->focusable && *found_current)
        return w;

    if (w == current)
        *found_current = 1;

    for (int i = 0; i < w->nchildren; i++) {
        glyph_widget_t *result = find_next_focusable(w->children[i], found_current, current);
        if (result)
            return result;
    }

    return NULL;
}

static glyph_widget_t *
find_first_focusable(glyph_widget_t *w)
{
    if (!w || !w->visible)
        return NULL;
    if (w->focusable)
        return w;
    for (int i = 0; i < w->nchildren; i++) {
        glyph_widget_t *result = find_first_focusable(w->children[i]);
        if (result)
            return result;
    }
    return NULL;
}

glyph_widget_t *
glyph_widget_focus_next(glyph_widget_t *root, glyph_widget_t *current)
{
    if (!root)
        return NULL;
    if (!current)
        return find_first_focusable(root);

    int found = 0;
    glyph_widget_t *next = find_next_focusable(root, &found, current);
    if (next)
        return next;

    /* Wrap around to first focusable */
    return find_first_focusable(root);
}

/* ---- Draw tree ---- */

void
glyph_widget_draw_tree(glyph_widget_t *root, surface_t *surf, int ox, int oy)
{
    if (!root || !root->visible)
        return;

    int ax = ox + root->x;
    int ay = oy + root->y;

    if (root->draw_fn)
        root->draw_fn(root, surf, ax, ay);

    for (int i = 0; i < root->nchildren; i++)
        glyph_widget_draw_tree(root->children[i], surf, ax, ay);

    root->dirty = 0;
}

/* ---- Destroy tree ---- */

void
glyph_widget_destroy_tree(glyph_widget_t *root)
{
    if (!root)
        return;
    for (int i = 0; i < root->nchildren; i++)
        glyph_widget_destroy_tree(root->children[i]);

    if (root->on_destroy)
        root->on_destroy(root);

    free(root);
}

/* ---- Set window pointer recursively ---- */

void
glyph_widget_set_window(glyph_widget_t *root, glyph_window_t *win)
{
    if (!root)
        return;
    root->window = win;
    for (int i = 0; i < root->nchildren; i++)
        glyph_widget_set_window(root->children[i], win);
}
