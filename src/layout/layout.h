#ifndef NB_LAYOUT_H
#define NB_LAYOUT_H

#include "../html/dom.h"
#include "../css/css.h"

/*
 * Layout engine: converts DOM+CSS → positioned boxes with (x,y,w,h).
 * Each DOM element gets a layout box. Supports:
 *  - Block layout (vertical stacking)
 *  - Inline layout (horizontal flow with line wrapping)
 *  - Flexbox (row/column, grow/shrink, justify/align)
 *  - Floats (basic, no shape-outside)
 *  - Absolute/fixed positioning
 *  - Tables (minimal: row/cell)
 */

typedef enum {
    BOX_BLOCK, BOX_INLINE, BOX_TEXT, BOX_FLEX, BOX_TABLE,
    BOX_TABLE_ROW, BOX_TABLE_CELL, BOX_INLINE_BLOCK
} NbBoxType;

typedef struct NbBox {
    NbBoxType      type;
    NbNode        *node;          /* owning DOM node */
    NbStyle       *style;         /* computed style */

    /* Geometry (all in pixels) */
    float x, y, width, height;    /* content box */
    float margin_top, margin_right, margin_bottom, margin_left;
    float padding_top, padding_right, padding_bottom, padding_left;
    float border_top, border_right, border_bottom, border_left;

    /* Text content for text boxes */
    char         *text;
    size_t        text_len;

    /* Tree */
    struct NbBox *parent;
    struct NbBox *first_child;
    struct NbBox *last_child;
    struct NbBox *next_sibling;
    struct NbBox *prev_sibling;
} NbBox;

typedef struct {
    NbBox    *root;       /* viewport box */
    NbArena  *arena;      /* all boxes live here */
    float     viewport_width;
    float     viewport_height;
} NbLayout;

/* Build layout tree from DOM */
NbLayout *nb_layout_build(NbDocument *doc, float vw, float vh);
void      nb_layout_free(NbLayout *layout);

/* Reflow (recalculate positions after resize) */
void nb_layout_reflow(NbLayout *layout, float vw, float vh);

/* Hit-test: find box at (x,y) */
NbBox *nb_layout_box_at(NbLayout *layout, float x, float y);

#endif /* NB_LAYOUT_H */
