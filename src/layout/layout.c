/*
 * layout.c — Box model layout engine
 *
 * Coordinate system: ALL positions are ABSOLUTE (from viewport top-left).
 * box->x, box->y are absolute pixel positions of the content area.
 * paint_box passes ox=0,oy=0 always; coords are already absolute.
 */
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
    else                    parent->first_child = child;
    parent->last_child  = child;
    child->next_sibling = NULL;
}

/* ------------------------------------------------------------------ */
/*  Length resolution                                                  */
/* ------------------------------------------------------------------ */
static float resolve(CssLength l, float ref, float fs, float vw, float vh) {
    return nb_css_resolve_length(l, ref, fs, vw, vh);
}

/* ------------------------------------------------------------------ */
/*  Text width estimate (character-based, Pango measures at render time) */
/* ------------------------------------------------------------------ */
static float estimate_text_w(const char *text, size_t len, float font_size) {
    /* Average char width ≈ 0.55 × font_size for proportional fonts */
    return (float)len * font_size * 0.55f;
}

/* ------------------------------------------------------------------ */
/*  Get effective style for a box (walk up if needed)                 */
/* ------------------------------------------------------------------ */
static NbStyle *eff_style(NbBox *box) {
    return box->style;
}

/* ------------------------------------------------------------------ */
/*  Resolve box margins/padding/borders from style                    */
/* ------------------------------------------------------------------ */
static void resolve_box_model(NbBox *box, float contain_w, float vw, float vh) {
    NbStyle *s = box->style;
    if (!s) return;
    float fs = s->font_size > 0 ? s->font_size : 16.0f;

    box->margin_top    = fmaxf(0, resolve(s->margin_top,    contain_w, fs, vw, vh));
    box->margin_bottom = fmaxf(0, resolve(s->margin_bottom, contain_w, fs, vw, vh));
    box->margin_left   = fmaxf(0, resolve(s->margin_left,   contain_w, fs, vw, vh));
    box->margin_right  = fmaxf(0, resolve(s->margin_right,  contain_w, fs, vw, vh));

    box->padding_top    = fmaxf(0, resolve(s->padding_top,    contain_w, fs, vw, vh));
    box->padding_bottom = fmaxf(0, resolve(s->padding_bottom, contain_w, fs, vw, vh));
    box->padding_left   = fmaxf(0, resolve(s->padding_left,   contain_w, fs, vw, vh));
    box->padding_right  = fmaxf(0, resolve(s->padding_right,  contain_w, fs, vw, vh));

    box->border_top    = (s->border_top.style    != BORDER_STYLE_NONE) ? fmaxf(1, s->border_top.width.value)    : 0;
    box->border_bottom = (s->border_bottom.style != BORDER_STYLE_NONE) ? fmaxf(1, s->border_bottom.width.value) : 0;
    box->border_left   = (s->border_left.style   != BORDER_STYLE_NONE) ? fmaxf(1, s->border_left.width.value)   : 0;
    box->border_right  = (s->border_right.style  != BORDER_STYLE_NONE) ? fmaxf(1, s->border_right.width.value)  : 0;
}

/* ------------------------------------------------------------------ */
/*  Forward declarations                                               */
/* ------------------------------------------------------------------ */
static float layout_block(NbBox *box, float abs_x, float abs_y,
                            float contain_w, float vw, float vh);
static float layout_flex(NbBox *box, float abs_x, float abs_y,
                          float contain_w, float vw, float vh);

/* ------------------------------------------------------------------ */
/*  Inline layout helper — lay inline/text children on lines          */
/* ------------------------------------------------------------------ */
static float layout_inline_children(NbBox *parent, float abs_x, float abs_y,
                                     float contain_w, float vw, float vh) {
    NbStyle *ps = parent->style;
    float line_h = (ps && ps->line_height > 0) ? ps->line_height
                  : (ps && ps->font_size > 0)  ? ps->font_size * 1.4f : 20.0f;
    float fs     = (ps && ps->font_size > 0)   ? ps->font_size : 16.0f;

    float cx = 0;   /* current x offset within line, relative to content box */
    float cy = 0;   /* current y offset, relative to content box */
    float cur_line_h = line_h;

    for (NbBox *c = parent->first_child; c; c = c->next_sibling) {
        float cw_est = 0;
        float ch_est = line_h;

        if (c->type == BOX_TEXT) {
            cw_est = estimate_text_w(c->text, c->text_len, fs);
            ch_est = line_h;
        } else {
            /* inline-block or unknown inline — give it estimated size */
            NbStyle *cs = c->style;
            if (cs) {
                float inner_w = (cs->width.unit != CSS_UNIT_AUTO)
                                ? resolve(cs->width, contain_w, cs->font_size, vw, vh)
                                : fminf(200.0f, contain_w);
                cw_est = inner_w;
                ch_est = (cs->height.unit != CSS_UNIT_AUTO)
                         ? resolve(cs->height, contain_w, cs->font_size, vw, vh) : line_h;
            } else {
                cw_est = 80; ch_est = line_h;
            }
        }

        /* Wrap to next line if needed */
        if (cx + cw_est > contain_w && cx > 0) {
            cy += cur_line_h;
            cx = 0;
            cur_line_h = line_h;
        }

        /* Position is absolute */
        c->x      = abs_x + cx;
        c->y      = abs_y + cy;
        c->width  = cw_est;
        c->height = ch_est;

        if (ch_est > cur_line_h) cur_line_h = ch_est;
        cx += cw_est;
    }

    return cy + cur_line_h; /* total height used */
}

/* ------------------------------------------------------------------ */
/*  Block layout — returns total outer height (including margins)     */
/* ------------------------------------------------------------------ */
static float layout_block(NbBox *box, float abs_x, float abs_y,
                            float contain_w, float vw, float vh) {
    NbStyle *s = box->style;
    if (!s) {
        /* document root — recurse only */
        box->x = abs_x; box->y = abs_y;
        box->width = contain_w; box->height = 0;
        float cy = abs_y;
        for (NbBox *c = box->first_child; c; c = c->next_sibling) {
            float ch = layout_block(c, abs_x, cy, contain_w, vw, vh);
            cy += ch;
        }
        box->height = cy - abs_y;
        return box->height;
    }

    float fs = s->font_size > 0 ? s->font_size : 16.0f;

    resolve_box_model(box, contain_w, vw, vh);

    /* Account for margin collapse (simplified: just use the margin) */
    float outer_x = abs_x + box->margin_left;
    float outer_y = abs_y + box->margin_top;

    /* Content width */
    float avail_w = contain_w - box->margin_left - box->margin_right
                               - box->border_left - box->border_right
                               - box->padding_left - box->padding_right;
    if (avail_w < 0) avail_w = 0;

    if (s->width.unit != CSS_UNIT_AUTO) {
        float explicit_w = resolve(s->width, contain_w, fs, vw, vh);
        if (explicit_w >= 0) avail_w = explicit_w;
    }

    /* Content box starts after border+padding */
    float content_x = outer_x + box->border_left + box->padding_left;
    float content_y = outer_y + box->border_top  + box->padding_top;

    /* Store absolute position of content box */
    box->x     = content_x;
    box->y     = content_y;
    box->width = avail_w;

    /* Check if children are purely inline */
    int has_block_child = 0;
    for (NbBox *c = box->first_child; c; c = c->next_sibling) {
        if (c->type == BOX_BLOCK || c->type == BOX_FLEX ||
            c->type == BOX_TABLE || c->type == BOX_TABLE_ROW) {
            has_block_child = 1; break;
        }
    }

    float content_h = 0;

    if (!has_block_child && box->first_child) {
        /* Inline formatting context */
        content_h = layout_inline_children(box, content_x, content_y, avail_w, vw, vh);
    } else {
        /* Block formatting context — stack children vertically */
        float cy = content_y;
        for (NbBox *c = box->first_child; c; c = c->next_sibling) {
            float child_outer_h;
            if (c->type == BOX_FLEX)
                child_outer_h = layout_flex(c, content_x, cy, avail_w, vw, vh);
            else if (c->type == BOX_TEXT) {
                /* Stray text in block context */
                NbStyle *cs = c->style ? c->style : s;
                float lh = cs->line_height > 0 ? cs->line_height : cs->font_size * 1.4f;
                c->x = content_x; c->y = cy;
                c->width  = estimate_text_w(c->text, c->text_len, cs->font_size);
                c->height = lh;
                child_outer_h = lh;
            } else {
                child_outer_h = layout_block(c, content_x, cy, avail_w, vw, vh);
                child_outer_h += c->margin_top + c->margin_bottom;
            }
            cy += child_outer_h;
        }
        content_h = cy - content_y;
    }

    /* Explicit height overrides */
    if (s->height.unit != CSS_UNIT_AUTO) {
        float explicit_h = resolve(s->height, 0, fs, vw, vh);
        if (explicit_h >= 0) content_h = explicit_h;
    }

    box->height = fmaxf(0, content_h);

    /* Return outer height for parent to use */
    float outer_h = box->border_top + box->padding_top
                  + box->height
                  + box->padding_bottom + box->border_bottom;
    return outer_h;
}

/* ------------------------------------------------------------------ */
/*  Flex layout                                                        */
/* ------------------------------------------------------------------ */
static float layout_flex(NbBox *box, float abs_x, float abs_y,
                          float contain_w, float vw, float vh) {
    NbStyle *s = box->style;
    if (!s) return 0;

    resolve_box_model(box, contain_w, vw, vh);

    float fs = s->font_size > 0 ? s->font_size : 16.0f;
    float avail_w = contain_w - box->margin_left - box->margin_right
                               - box->border_left - box->border_right
                               - box->padding_left - box->padding_right;
    if (avail_w < 0) avail_w = 0;
    if (s->width.unit != CSS_UNIT_AUTO) {
        float ew = resolve(s->width, contain_w, fs, vw, vh);
        if (ew >= 0) avail_w = ew;
    }

    float content_x = abs_x + box->margin_left + box->border_left + box->padding_left;
    float content_y = abs_y + box->margin_top  + box->border_top  + box->padding_top;
    box->x = content_x; box->y = content_y; box->width = avail_w;

    int is_row = (s->flex_direction == FLEX_DIR_ROW ||
                  s->flex_direction == FLEX_DIR_ROW_REVERSE);

    float max_h = 0;
    if (is_row) {
        float cx = content_x;
        for (NbBox *c = box->first_child; c; c = c->next_sibling) {
            float ch = layout_block(c, cx, content_y, avail_w / fmaxf(1, box->width), vw, vh);
            float cw2 = c->width + c->border_left + c->border_right + c->padding_left + c->padding_right + c->margin_left + c->margin_right;
            cx += cw2;
            if (ch > max_h) max_h = ch;
        }
    } else {
        float cy = content_y;
        for (NbBox *c = box->first_child; c; c = c->next_sibling) {
            float ch = layout_block(c, content_x, cy, avail_w, vw, vh);
            cy += ch;
        }
        max_h = cy - content_y;
    }

    if (s->height.unit != CSS_UNIT_AUTO) {
        float eh = resolve(s->height, 0, fs, vw, vh);
        if (eh >= 0) max_h = eh;
    }
    box->height = fmaxf(0, max_h);

    return box->border_top + box->padding_top + box->height
         + box->padding_bottom + box->border_bottom;
}

/* ------------------------------------------------------------------ */
/*  Build box tree from DOM                                            */
/* ------------------------------------------------------------------ */
static NbBox *build_box(NbArena *a, NbNode *node, NbBox *parent_box) {
    if (!node) return NULL;

    if (node->type == NB_NODE_TEXT) {
        if (!node->text || !node->text[0]) return NULL;
        /* Skip whitespace-only nodes in block context */
        int all_ws = 1;
        for (const char *p = node->text; *p; p++)
            if (*p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') { all_ws = 0; break; }
        if (all_ws) return NULL;

        NbBox *tb = box_new(a, node, BOX_TEXT);
        tb->text     = node->text;
        tb->text_len = strlen(node->text);
        /* Inherit style from parent */
        if (parent_box) tb->style = parent_box->style;
        return tb;
    }

    if (node->type != NB_NODE_ELEMENT) {
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
/*  Public API                                                         */
/* ------------------------------------------------------------------ */
NbLayout *nb_layout_build(NbDocument *doc, float vw, float vh) {
    NbLayout *layout = calloc(1, sizeof(*layout));
    layout->arena           = nb_arena_new(0);
    layout->viewport_width  = vw;
    layout->viewport_height = vh;

    layout->root = build_box(layout->arena, doc->root, NULL);
    if (!layout->root) { nb_layout_free(layout); return NULL; }

    /* Layout from absolute origin (0,0) */
    layout->root->x = 0; layout->root->y = 0;
    layout->root->width = vw;
    layout_block(layout->root, 0, 0, vw, vw, vh);

    return layout;
}

void nb_layout_reflow(NbLayout *layout, float vw, float vh) {
    if (!layout || !layout->root) return;
    layout->viewport_width  = vw;
    layout->viewport_height = vh;
    layout->root->x = 0; layout->root->y = 0;
    layout->root->width = vw;
    layout_block(layout->root, 0, 0, vw, vw, vh);
}

void nb_layout_free(NbLayout *layout) {
    if (!layout) return;
    nb_arena_free(layout->arena);
    free(layout);
}

/* Hit-test: find deepest box at (x,y) — coords are absolute */
NbBox *nb_layout_box_at(NbLayout *layout, float x, float y) {
    if (!layout || !layout->root) return NULL;
    NbBox *best = NULL;
    NbBox *stack[512]; int top = 0;
    stack[top++] = layout->root;
    while (top > 0) {
        NbBox *b = stack[--top];
        /* Box rect: x,y is content origin; include padding/border for hit area */
        float bx = b->x - b->border_left - b->padding_left;
        float by = b->y - b->border_top  - b->padding_top;
        float bw = b->width  + b->border_left + b->border_right + b->padding_left + b->padding_right;
        float bh = b->height + b->border_top  + b->border_bottom + b->padding_top + b->padding_bottom;
        if (x >= bx && x <= bx+bw && y >= by && y <= by+bh) {
            best = b;
            for (NbBox *c = b->first_child; c; c = c->next_sibling)
                if (top < 511) stack[top++] = c;
        }
    }
    return best;
}
