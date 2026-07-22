#include "layout.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* ------------------------------------------------------------------ */
/*  Box allocation                                                     */
/* ------------------------------------------------------------------ */
static NbBox *box_new(NbArena *a, NbNode *node, NbBoxType type) {
    NbBox *b = nb_arena_alloc0(a, sizeof(NbBox));
    b->node  = node;
    b->type  = type;
    b->style = node ? (NbStyle*)node->computed_style : NULL;
    return b;
}

static void box_append(NbBox *parent, NbBox *child) {
    child->parent = parent;
    child->prev_sibling = parent->last_child;
    if (parent->last_child) parent->last_child->next_sibling = child;
    else                     parent->first_child = child;
    parent->last_child  = child;
    child->next_sibling = NULL;
}

/* ------------------------------------------------------------------ */
/*  Length resolution helpers                                          */
/* ------------------------------------------------------------------ */
static float resolve(CssLength l, float ref, float fs, float vw, float vh) {
    return nb_css_resolve_length(l, ref, fs, vw, vh);
}

static float box_border_h(NbBox *b) { return b->border_left + b->border_right; }
static float box_border_v(NbBox *b) { return b->border_top  + b->border_bottom; }
static float box_padding_h(NbBox *b){ return b->padding_left + b->padding_right; }
static float box_padding_v(NbBox *b){ return b->padding_top  + b->padding_bottom; }
static float box_margin_h(NbBox *b) { return b->margin_left  + b->margin_right; }

/* ------------------------------------------------------------------ */
/*  Build box tree from DOM                                            */
/* ------------------------------------------------------------------ */
static NbBox *build_box(NbArena *a, NbNode *node, NbBox *parent_box) {
    if (!node) return NULL;

    if (node->type == NODE_TEXT) {
        /* Text nodes — only if non-empty */
        if (!node->text || !node->text[0]) return NULL;
        /* Skip pure-whitespace when parent is block */
        int all_ws = 1;
        for (const char *p = node->text; *p; p++)
            if (*p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') { all_ws = 0; break; }
        if (all_ws && parent_box && parent_box->type == BOX_BLOCK) return NULL;

        NbBox *tb = box_new(a, node, BOX_TEXT);
        tb->text     = node->text;
        tb->text_len = strlen(node->text);
        /* Inherit style from parent box */
        if (parent_box) tb->style = parent_box->style;
        return tb;
    }

    if (node->type != NODE_ELEMENT) {
        /* recurse into document node */
        NbBox *dummy = box_new(a, node, BOX_BLOCK);
        for (NbNode *c = node->first_child; c; c = c->next_sibling) {
            NbBox *cb = build_box(a, c, dummy);
            if (cb) box_append(dummy, cb);
        }
        return dummy;
    }

    NbStyle *st = (NbStyle*)node->computed_style;
    if (!st || st->display == DISPLAY_NONE) return NULL;

    NbBoxType btype;
    switch (st->display) {
        case DISPLAY_FLEX:         btype = BOX_FLEX;         break;
        case DISPLAY_INLINE_BLOCK: btype = BOX_INLINE_BLOCK; break;
        case DISPLAY_TABLE:        btype = BOX_TABLE;        break;
        case DISPLAY_TABLE_ROW:    btype = BOX_TABLE_ROW;    break;
        case DISPLAY_TABLE_CELL:   btype = BOX_TABLE_CELL;   break;
        case DISPLAY_INLINE:       btype = BOX_INLINE;       break;
        default:                   btype = BOX_BLOCK;        break;
    }

    NbBox *b = box_new(a, node, btype);

    for (NbNode *c = node->first_child; c; c = c->next_sibling) {
        NbBox *cb = build_box(a, c, b);
        if (cb) box_append(b, cb);
    }
    return b;
}

/* ------------------------------------------------------------------ */
/*  Text width measurement (very simplified, assumes monospace 8px)   */
/* ------------------------------------------------------------------ */
static float measure_text_width(const char *text, size_t len, NbStyle *style) {
    /* In real impl, use font metrics (Pango, FreeType, etc.) */
    float char_width = style ? style->font_size * 0.6f : 8.0f;
    return (float)len * char_width;
}

/* ------------------------------------------------------------------ */
/*  Block layout                                                       */
/* ------------------------------------------------------------------ */
static void layout_block(NbBox *box, float contain_w, float vw, float vh) {
    NbStyle *s = box->style;
    if (!s) return;

    /* Resolve margins, padding, border */
    box->margin_top    = fmaxf(0, resolve(s->margin_top,    contain_w, s->font_size, vw, vh));
    box->margin_bottom = fmaxf(0, resolve(s->margin_bottom, contain_w, s->font_size, vw, vh));
    box->margin_left   = fmaxf(0, resolve(s->margin_left,   contain_w, s->font_size, vw, vh));
    box->margin_right  = fmaxf(0, resolve(s->margin_right,  contain_w, s->font_size, vw, vh));

    box->padding_top    = fmaxf(0, resolve(s->padding_top,    contain_w, s->font_size, vw, vh));
    box->padding_bottom = fmaxf(0, resolve(s->padding_bottom, contain_w, s->font_size, vw, vh));
    box->padding_left   = fmaxf(0, resolve(s->padding_left,   contain_w, s->font_size, vw, vh));
    box->padding_right  = fmaxf(0, resolve(s->padding_right,  contain_w, s->font_size, vw, vh));

    box->border_top    = s->border_top.width.value;
    box->border_bottom = s->border_bottom.width.value;
    box->border_left   = s->border_left.width.value;
    box->border_right  = s->border_right.width.value;

    /* Width resolution */
    float auto_w = contain_w - box_margin_h(box) - box_border_h(box) - box_padding_h(box);
    if (s->width.unit == CSS_UNIT_AUTO) box->width = fmaxf(0, auto_w);
    else box->width = resolve(s->width, contain_w, s->font_size, vw, vh);

    /* Height resolution (auto computed after children) */
    if (s->height.unit == CSS_UNIT_AUTO) box->height = 0; /* computed below */
    else box->height = resolve(s->height, contain_w, s->font_size, vw, vh);

    /* Layout children (block-level stack vertically) */
    float cy = box->padding_top + box->border_top;
    float cw = box->width;
    for (NbBox *c = box->first_child; c; c = c->next_sibling) {
        c->x = box->padding_left + box->border_left;
        c->y = cy;
        if (c->type == BOX_TEXT) {
            /* text node — measure width */
            c->width  = measure_text_width(c->text, c->text_len, c->style);
            c->height = c->style ? c->style->line_height : 16.0f;
            cy += c->height;
        } else if (c->type == BOX_INLINE || c->type == BOX_INLINE_BLOCK) {
            /* inline children — simplified flow */
            layout_block(c, cw, vw, vh);
            cy += c->height + c->margin_top + c->margin_bottom;
        } else {
            layout_block(c, cw, vw, vh);
            cy += c->height + c->margin_top + c->margin_bottom +
                  c->border_top + c->border_bottom + c->padding_top + c->padding_bottom;
        }
    }
    cy += box->padding_bottom + box->border_bottom;

    if (s->height.unit == CSS_UNIT_AUTO) box->height = cy - box->border_top - box->padding_top;
}

/* ------------------------------------------------------------------ */
/*  Flexbox layout (basic row, no wrapping)                           */
/* ------------------------------------------------------------------ */
static void layout_flex(NbBox *box, float contain_w, float vw, float vh) {
    NbStyle *s = box->style;
    if (!s) return;

    /* margins/padding/border same as block */
    box->margin_top    = fmaxf(0, resolve(s->margin_top,    contain_w, s->font_size, vw, vh));
    box->margin_bottom = fmaxf(0, resolve(s->margin_bottom, contain_w, s->font_size, vw, vh));
    box->margin_left   = fmaxf(0, resolve(s->margin_left,   contain_w, s->font_size, vw, vh));
    box->margin_right  = fmaxf(0, resolve(s->margin_right,  contain_w, s->font_size, vw, vh));
    box->padding_top    = fmaxf(0, resolve(s->padding_top,    contain_w, s->font_size, vw, vh));
    box->padding_bottom = fmaxf(0, resolve(s->padding_bottom, contain_w, s->font_size, vw, vh));
    box->padding_left   = fmaxf(0, resolve(s->padding_left,   contain_w, s->font_size, vw, vh));
    box->padding_right  = fmaxf(0, resolve(s->padding_right,  contain_w, s->font_size, vw, vh));
    box->border_top  = s->border_top.width.value;
    box->border_bottom = s->border_bottom.width.value;
    box->border_left = s->border_left.width.value;
    box->border_right= s->border_right.width.value;

    float auto_w = contain_w - box_margin_h(box) - box_border_h(box) - box_padding_h(box);
    if (s->width.unit == CSS_UNIT_AUTO) box->width = fmaxf(0, auto_w);
    else box->width = resolve(s->width, contain_w, s->font_size, vw, vh);

    if (s->height.unit == CSS_UNIT_AUTO) box->height = 0;
    else box->height = resolve(s->height, contain_w, s->font_size, vw, vh);

    /* Flex row: lay children horizontally */
    float cx = box->padding_left + box->border_left;
    float cy = box->padding_top  + box->border_top;
    float max_h = 0;
    for (NbBox *c = box->first_child; c; c = c->next_sibling) {
        c->x = cx;
        c->y = cy;
        if (c->type == BOX_TEXT) {
            c->width  = measure_text_width(c->text, c->text_len, c->style);
            c->height = c->style ? c->style->line_height : 16.0f;
        } else {
            layout_block(c, box->width, vw, vh);
        }
        cx += c->width + c->margin_left + c->margin_right;
        if (c->height > max_h) max_h = c->height;
    }
    if (s->height.unit == CSS_UNIT_AUTO) box->height = max_h + box->padding_top + box->padding_bottom;
}

/* ------------------------------------------------------------------ */
/*  Dispatch layout to the right algorithm                            */
/* ------------------------------------------------------------------ */
static void layout_box(NbBox *box, float contain_w, float vw, float vh) {
    if (!box) return;
    if (!box->style && box->type != BOX_TEXT) {
        /* document root */
        for (NbBox *c = box->first_child; c; c = c->next_sibling)
            layout_box(c, contain_w, vw, vh);
        return;
    }
    switch (box->type) {
        case BOX_FLEX:       layout_flex(box, contain_w, vw, vh); break;
        case BOX_TEXT:
            box->width  = measure_text_width(box->text, box->text_len, box->style);
            box->height = box->style ? box->style->line_height : 16.0f;
            break;
        default:             layout_block(box, contain_w, vw, vh); break;
    }
}

/* Absolute/fixed positioning pass */
static void position_abs(NbBox *box, float vw, float vh) {
    if (!box || !box->style) return;
    NbStyle *s = box->style;
    if (s->position == POSITION_ABSOLUTE || s->position == POSITION_FIXED) {
        float ref_x = 0, ref_y = 0, ref_w = vw, ref_h = vh;
        if (s->position == POSITION_ABSOLUTE && box->parent) {
            ref_x = box->parent->x;
            ref_y = box->parent->y;
            ref_w = box->parent->width;
            ref_h = box->parent->height;
        }
        if (s->left.unit != CSS_UNIT_AUTO)
            box->x = ref_x + resolve(s->left, ref_w, s->font_size, vw, vh);
        if (s->top.unit != CSS_UNIT_AUTO)
            box->y = ref_y + resolve(s->top, ref_h, s->font_size, vw, vh);
        if (s->right.unit != CSS_UNIT_AUTO && s->width.unit == CSS_UNIT_AUTO)
            box->width = ref_w - box->x - resolve(s->right, ref_w, s->font_size, vw, vh);
        if (s->bottom.unit != CSS_UNIT_AUTO && s->height.unit == CSS_UNIT_AUTO)
            box->height = ref_h - box->y - resolve(s->bottom, ref_h, s->font_size, vw, vh);
    }
    for (NbBox *c = box->first_child; c; c = c->next_sibling)
        position_abs(c, vw, vh);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */
NbLayout *nb_layout_build(NbDocument *doc, float vw, float vh) {
    NbLayout *layout = calloc(1, sizeof(*layout));
    layout->arena          = nb_arena_new(0);
    layout->viewport_width  = vw;
    layout->viewport_height = vh;

    layout->root = build_box(layout->arena, doc->root, NULL);
    if (!layout->root) { nb_layout_free(layout); return NULL; }

    layout->root->x     = 0;
    layout->root->y     = 0;
    layout->root->width = vw;

    layout_box(layout->root, vw, vw, vh);
    position_abs(layout->root, vw, vh);

    return layout;
}

void nb_layout_reflow(NbLayout *layout, float vw, float vh) {
    if (!layout) return;
    layout->viewport_width  = vw;
    layout->viewport_height = vh;
    if (!layout->root) return;
    layout->root->x     = 0;
    layout->root->y     = 0;
    layout->root->width = vw;
    layout_box(layout->root, vw, vw, vh);
    position_abs(layout->root, vw, vh);
}

void nb_layout_free(NbLayout *layout) {
    if (!layout) return;
    nb_arena_free(layout->arena);
    free(layout);
}

NbBox *nb_layout_box_at(NbLayout *layout, float x, float y) {
    if (!layout || !layout->root) return NULL;
    /* DFS, find deepest matching box */
    NbBox *stack[512]; int top = 0;
    NbBox *best = NULL;
    stack[top++] = layout->root;
    while (top > 0) {
        NbBox *b = stack[--top];
        float bx = b->x - b->border_left - b->padding_left;
        float by = b->y - b->border_top  - b->padding_top;
        float bw = b->width  + box_border_h(b) + box_padding_h(b);
        float bh = b->height + box_border_v(b) + box_padding_v(b);
        if (x >= bx && x <= bx+bw && y >= by && y <= by+bh) {
            best = b;
            for (NbBox *c = b->first_child; c; c = c->next_sibling)
                if (top < 511) stack[top++] = c;
        }
    }
    return best;
}
