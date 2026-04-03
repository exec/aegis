/* widget_test.c — Glyph widget showcase window for Lumen compositor.
 *
 * Creates a tabbed window exercising every Glyph widget type:
 *   Tab 1 (Text):     labels, text fields
 *   Tab 2 (Controls): buttons, checkboxes, progress bar
 *   Tab 3 (Lists):    list view with scrollbar
 *   Tab 4 (Layout):   nested hbox/vbox demo
 */
#include <glyph.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ---- Shared state ---------------------------------------------------- */

static glyph_label_t   *s_status_label;
static glyph_progress_t *s_progress;
static int               s_progress_val;

/* ---- Callbacks ------------------------------------------------------- */

static void
on_button_click(glyph_widget_t *self)
{
    (void)self;
    if (s_status_label)
        glyph_label_set_text(s_status_label, "Button clicked!");
}

static void
on_button_disabled(glyph_widget_t *self)
{
    (void)self;
    /* disabled button — should never fire */
}

static void
on_checkbox_change(glyph_widget_t *self, int checked)
{
    (void)self;
    if (s_status_label)
        glyph_label_set_text(s_status_label,
                             checked ? "Checkbox: ON" : "Checkbox: OFF");
}

static void
on_textfield_change(glyph_widget_t *self, const char *text)
{
    (void)self;
    if (s_status_label) {
        static char msg[80];
        snprintf(msg, sizeof(msg), "Text: %.60s", text);
        glyph_label_set_text(s_status_label, msg);
    }
}

static void
on_list_select(glyph_widget_t *self, int index)
{
    (void)self;
    if (s_status_label) {
        static char msg[80];
        snprintf(msg, sizeof(msg), "Selected item %d", index);
        glyph_label_set_text(s_status_label, msg);
    }
}

static void
on_progress_click(glyph_widget_t *self)
{
    (void)self;
    s_progress_val = (s_progress_val + 10) % 110;
    if (s_progress_val > 100) s_progress_val = 0;
    if (s_progress)
        glyph_progress_set_value(s_progress, s_progress_val);
    if (s_status_label) {
        static char msg[40];
        snprintf(msg, sizeof(msg), "Progress: %d%%", s_progress_val);
        glyph_label_set_text(s_status_label, msg);
    }
}

static void
on_tab_change(glyph_widget_t *self, int index)
{
    (void)self;
    if (s_status_label) {
        static const char *names[] = {"Text", "Controls", "Lists", "Layout"};
        static char msg[40];
        snprintf(msg, sizeof(msg), "Tab: %s", index < 4 ? names[index] : "?");
        glyph_label_set_text(s_status_label, msg);
    }
}

/* ---- Tab builders ---------------------------------------------------- */

static glyph_widget_t *
build_text_tab(void)
{
    glyph_box_t *vbox = glyph_vbox_create(8, 6);

    glyph_label_t *title = glyph_label_create("Text Widgets", C_ACCENT);
    glyph_widget_add_child((glyph_widget_t *)vbox, (glyph_widget_t *)title);

    glyph_label_t *l1 = glyph_label_create("Default label (white)", C_WIN);
    glyph_widget_add_child((glyph_widget_t *)vbox, (glyph_widget_t *)l1);

    glyph_label_t *l2 = glyph_label_create("Colored label (green)", C_GREEN);
    glyph_widget_add_child((glyph_widget_t *)vbox, (glyph_widget_t *)l2);

    glyph_label_t *l3 = glyph_label_create("Subtle label (gray)", C_SUBTLE);
    glyph_widget_add_child((glyph_widget_t *)vbox, (glyph_widget_t *)l3);

    glyph_label_t *sep = glyph_label_create("--- Text Fields ---", C_BAR_T);
    glyph_widget_add_child((glyph_widget_t *)vbox, (glyph_widget_t *)sep);

    glyph_textfield_t *tf1 = glyph_textfield_create(30, on_textfield_change);
    glyph_widget_add_child((glyph_widget_t *)vbox, (glyph_widget_t *)tf1);

    glyph_textfield_t *tf2 = glyph_textfield_create(20, NULL);
    glyph_widget_add_child((glyph_widget_t *)vbox, (glyph_widget_t *)tf2);

    return (glyph_widget_t *)vbox;
}

static glyph_widget_t *
build_controls_tab(void)
{
    glyph_box_t *vbox = glyph_vbox_create(8, 6);

    glyph_label_t *title = glyph_label_create("Control Widgets", C_ACCENT);
    glyph_widget_add_child((glyph_widget_t *)vbox, (glyph_widget_t *)title);

    /* Buttons in a row */
    glyph_box_t *btn_row = glyph_hbox_create(0, 8);

    glyph_button_t *b1 = glyph_button_create("Click Me", on_button_click);
    glyph_widget_add_child((glyph_widget_t *)btn_row, (glyph_widget_t *)b1);

    glyph_button_t *b2 = glyph_button_create("Disabled", on_button_disabled);
    glyph_button_set_state(b2, GLYPH_BTN_DISABLED);
    glyph_widget_add_child((glyph_widget_t *)btn_row, (glyph_widget_t *)b2);

    glyph_button_t *b3 = glyph_button_create("Progress +10%", on_progress_click);
    glyph_widget_add_child((glyph_widget_t *)btn_row, (glyph_widget_t *)b3);

    glyph_widget_add_child((glyph_widget_t *)vbox, (glyph_widget_t *)btn_row);

    /* Checkboxes */
    glyph_checkbox_t *cb1 = glyph_checkbox_create("Enable feature A", on_checkbox_change);
    glyph_widget_add_child((glyph_widget_t *)vbox, (glyph_widget_t *)cb1);

    glyph_checkbox_t *cb2 = glyph_checkbox_create("Enable feature B", on_checkbox_change);
    glyph_widget_add_child((glyph_widget_t *)vbox, (glyph_widget_t *)cb2);

    /* Progress bar */
    glyph_label_t *pl = glyph_label_create("Progress Bar:", C_WIN);
    glyph_widget_add_child((glyph_widget_t *)vbox, (glyph_widget_t *)pl);

    s_progress = glyph_progress_create(300);
    s_progress_val = 35;
    glyph_progress_set_value(s_progress, s_progress_val);
    glyph_widget_add_child((glyph_widget_t *)vbox, (glyph_widget_t *)s_progress);

    return (glyph_widget_t *)vbox;
}

static glyph_widget_t *
build_lists_tab(void)
{
    glyph_box_t *vbox = glyph_vbox_create(8, 6);

    glyph_label_t *title = glyph_label_create("List Widgets", C_ACCENT);
    glyph_widget_add_child((glyph_widget_t *)vbox, (glyph_widget_t *)title);

    static const char *items[] = {
        "Capability System", "Virtual Memory Manager",
        "Process Scheduler", "ELF Loader",
        "ext2 Filesystem", "NVMe Driver",
        "TCP/IP Stack", "USB HID Keyboard",
        "USB HID Mouse", "Lumen Compositor",
        "Glyph Toolkit", "Bastion Login"
    };
    glyph_listview_t *lv = glyph_listview_create(280, 8, on_list_select);
    glyph_listview_set_items(lv, items, 12);
    glyph_widget_add_child((glyph_widget_t *)vbox, (glyph_widget_t *)lv);

    return (glyph_widget_t *)vbox;
}

static glyph_widget_t *
build_layout_tab(void)
{
    glyph_box_t *outer = glyph_vbox_create(8, 8);

    glyph_label_t *title = glyph_label_create("Layout Demo", C_ACCENT);
    glyph_widget_add_child((glyph_widget_t *)outer, (glyph_widget_t *)title);

    /* Row 1: three buttons in an hbox */
    glyph_box_t *row1 = glyph_hbox_create(4, 6);
    glyph_button_t *r1b1 = glyph_button_create("Left", on_button_click);
    glyph_button_t *r1b2 = glyph_button_create("Center", on_button_click);
    glyph_button_t *r1b3 = glyph_button_create("Right", on_button_click);
    glyph_widget_add_child((glyph_widget_t *)row1, (glyph_widget_t *)r1b1);
    glyph_widget_add_child((glyph_widget_t *)row1, (glyph_widget_t *)r1b2);
    glyph_widget_add_child((glyph_widget_t *)row1, (glyph_widget_t *)r1b3);
    glyph_widget_add_child((glyph_widget_t *)outer, (glyph_widget_t *)row1);

    /* Row 2: label + textfield + button */
    glyph_box_t *row2 = glyph_hbox_create(4, 6);
    glyph_label_t *r2l = glyph_label_create("Name:", C_WIN);
    glyph_textfield_t *r2tf = glyph_textfield_create(15, NULL);
    glyph_button_t *r2b = glyph_button_create("Submit", on_button_click);
    glyph_widget_add_child((glyph_widget_t *)row2, (glyph_widget_t *)r2l);
    glyph_widget_add_child((glyph_widget_t *)row2, (glyph_widget_t *)r2tf);
    glyph_widget_add_child((glyph_widget_t *)row2, (glyph_widget_t *)r2b);
    glyph_widget_add_child((glyph_widget_t *)outer, (glyph_widget_t *)row2);

    /* Row 3: nested vbox inside hbox */
    glyph_box_t *row3 = glyph_hbox_create(4, 12);

    glyph_box_t *col1 = glyph_vbox_create(4, 4);
    glyph_label_t *c1l = glyph_label_create("Column 1", C_YELLOW);
    glyph_checkbox_t *c1cb = glyph_checkbox_create("Option X", NULL);
    glyph_checkbox_t *c1cb2 = glyph_checkbox_create("Option Y", NULL);
    glyph_widget_add_child((glyph_widget_t *)col1, (glyph_widget_t *)c1l);
    glyph_widget_add_child((glyph_widget_t *)col1, (glyph_widget_t *)c1cb);
    glyph_widget_add_child((glyph_widget_t *)col1, (glyph_widget_t *)c1cb2);

    glyph_box_t *col2 = glyph_vbox_create(4, 4);
    glyph_label_t *c2l = glyph_label_create("Column 2", C_YELLOW);
    glyph_button_t *c2b1 = glyph_button_create("Action 1", on_button_click);
    glyph_button_t *c2b2 = glyph_button_create("Action 2", on_button_click);
    glyph_widget_add_child((glyph_widget_t *)col2, (glyph_widget_t *)c2l);
    glyph_widget_add_child((glyph_widget_t *)col2, (glyph_widget_t *)c2b1);
    glyph_widget_add_child((glyph_widget_t *)col2, (glyph_widget_t *)c2b2);

    glyph_widget_add_child((glyph_widget_t *)row3, (glyph_widget_t *)col1);
    glyph_widget_add_child((glyph_widget_t *)row3, (glyph_widget_t *)col2);
    glyph_widget_add_child((glyph_widget_t *)outer, (glyph_widget_t *)row3);

    return (glyph_widget_t *)outer;
}

/* ---- Public API ------------------------------------------------------ */

glyph_window_t *
widget_test_create(void)
{
    glyph_window_t *win = glyph_window_create("Test Widgets", 420, 380);
    if (!win) return NULL;

    /* Root: vbox with tabs + status bar */
    glyph_box_t *root = glyph_vbox_create(4, 4);

    /* Tabs widget */
    glyph_tabs_t *tabs = glyph_tabs_create(on_tab_change);
    glyph_tabs_add(tabs, "Text",     build_text_tab());
    glyph_tabs_add(tabs, "Controls", build_controls_tab());
    glyph_tabs_add(tabs, "Lists",    build_lists_tab());
    glyph_tabs_add(tabs, "Layout",   build_layout_tab());
    glyph_widget_add_child((glyph_widget_t *)root, (glyph_widget_t *)tabs);

    /* Status label at bottom */
    s_status_label = glyph_label_create("Ready", C_SUBTLE);
    glyph_widget_add_child((glyph_widget_t *)root, (glyph_widget_t *)s_status_label);

    glyph_window_set_content(win, (glyph_widget_t *)root);

    return win;
}
