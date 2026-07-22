/*
 * layout.c  —  Box layout engine
 *
 * ALL box positions (box->x, box->y) are ABSOLUTE pixel coords from
 * the viewport top-left (0,0).  The renderer uses them directly.
 *
 * layout_block() / layout_flex() return the OUTER height consumed
 * (border + padding + content + padding + border) NOT including margin.
 * The caller adds margins separately when advancing its cursor.
 */
#include "layout.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */
static NbBox *box_new(NbArena *a, NbNode *node, NbBoxType type) {
    NbBox *b   = nb_arena_alloc0(a, sizeof(NbBox));
    b->node    = node;
    b->type    = type;
    b->style   = node ? (NbStyle*)node->computed_style : NULL;
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
static float rl(CssLength l, float ref, float fs, float vw, float vh) {
    return nb_css_resolve_length(l, ref, fs, vw, vh);
}

/* ------------------------------------------------------------------ */
/*  Tags whose text content must never produce layout boxes           */
/* ------------------------------------------------------------------ */
static int tag_is_invisible(const char *tag) {
    static const char *inv[] = {
        "script","style","head","meta","link","title",
        "noscript","template","svg","math", NULL
    };
    if (!tag) return 0;
    for (int i = 0; inv[i]; i++)
        if (strcmp(inv[i], tag) == 0) return 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Resolve box-model spacing into box fields                         */
/* ------------------------------------------------------------------ */
static void resolve_spacing(NbBox *box, float cw, float vw, float vh) {
    NbStyle *s = box->style;
    if (!s) return;
    float fs = s->font_size > 0 ? s->font_size : 16.f;

    /* margins */
    float mt = rl(s->margin_top,    cw,fs,vw,vh); box->margin_top    = mt<0?0:mt;
    float mb = rl(s->margin_bottom, cw,fs,vw,vh); box->margin_bottom = mb<0?0:mb;
    float ml = rl(s->margin_left,   cw,fs,vw,vh); box->margin_left   = ml<0?0:ml;
    float mr = rl(s->margin_right,  cw,fs,vw,vh); box->margin_right  = mr<0?0:mr;

    /* padding */
    float pt = rl(s->padding_top,    cw,fs,vw,vh); box->padding_top    = pt<0?0:pt;
    float pb = rl(s->padding_bottom, cw,fs,vw,vh); box->padding_bottom = pb<0?0:pb;
    float pl = rl(s->padding_left,   cw,fs,vw,vh); box->padding_left   = pl<0?0:pl;
    float pr = rl(s->padding_right,  cw,fs,vw,vh); box->padding_right  = pr<0?0:pr;

    /* border — only non-zero if style is set */
    box->border_top    = s->border_top.style    != BORDER_STYLE_NONE ? fmaxf(0,s->border_top.width.value)    : 0;
    box->border_bottom = s->border_bottom.style != BORDER_STYLE_NONE ? fmaxf(0,s->border_bottom.width.value) : 0;
    box->border_left   = s->border_left.style   != BORDER_STYLE_NONE ? fmaxf(0,s->border_left.width.value)   : 0;
    box->border_right  = s->border_right.style  != BORDER_STYLE_NONE ? fmaxf(0,s->border_right.width.value)  : 0;
}

/* ------------------------------------------------------------------ */
/*  Estimate text width (used for inline flow; Pango renders actual)  */
/* ------------------------------------------------------------------ */
static float text_width_est(const char *text, size_t len, float fs) {
    return (float)len * (fs > 0 ? fs : 16.f) * 0.52f;
}

/* ------------------------------------------------------------------ */
/*  Forward declaration                                                */
/* ------------------------------------------------------------------ */
static float do_layout(NbBox *box, float ax, float ay,
                        float cw, float vw, float vh);

/* ------------------------------------------------------------------ */
/*  Inline formatting context                                          */
/*  Returns the HEIGHT consumed by laying inline children.            */
/* ------------------------------------------------------------------ */
static float layout_inline(NbBox *parent, float ax, float ay,
                             float avail_w, float vw, float vh) {
    NbStyle *ps  = parent->style;
    float base_fs = ps && ps->font_size > 0 ? ps->font_size : 16.f;
    float line_h  = ps && ps->line_height > 0
                    ? ps->line_height : base_fs * 1.4f;

    float cx = 0;        /* cursor x within line (0 = left edge of content box) */
    float cy = 0;        /* cursor y (top of current line, relative to content box) */
    float cur_lh = line_h;

    for (NbBox *c = parent->first_child; c; c = c->next_sibling) {
        float cw_est, ch_est;

        if (c->type == BOX_TEXT) {
            NbStyle *cs = c->style ? c->style : ps;
            float fs    = cs && cs->font_size > 0 ? cs->font_size : base_fs;
            float lh2   = cs && cs->line_height > 0 ? cs->line_height : fs * 1.4f;
            cw_est = text_width_est(c->text, c->text_len, fs);
            ch_est = lh2;
        } else {
            /* inline-block: do a full layout with 0-based origin, then record size */
            do_layout(c, 0, 0, avail_w, vw, vh);
            cw_est = c->width  + c->border_left + c->border_right
                               + c->padding_left + c->padding_right
                               + c->margin_left  + c->margin_right;
            ch_est = c->height + c->border_top  + c->border_bottom
                               + c->padding_top  + c->padding_bottom
                               + c->margin_top   + c->margin_bottom;
        }

        /* word-wrap: advance to next line if item doesn't fit */
        if (cw_est < avail_w && cx + cw_est > avail_w && cx > 0) {
            cy += cur_lh;
            cx  = 0;
            cur_lh = line_h;
        }

        c->x      = ax + cx;
        c->y      = ay + cy;
        c->width  = cw_est;
        c->height = ch_est;

        if (ch_est > cur_lh) cur_lh = ch_est;
        cx += cw_est;
    }
    return cy + cur_lh;   /* total height */
}

/* ------------------------------------------------------------------ */
/*  Main layout function                                               */
/*  ax, ay — absolute pixel origin of this box's MARGIN EDGE          */
/*  Returns outer height (excluding margin).                           */
/* ------------------------------------------------------------------ */
static float do_layout(NbBox *box, float ax, float ay,
                        float cw, float vw, float vh) {
    if (!box) return 0;

    /* ---- Document root (no style) ---- */
    if (!box->style) {
        box->x = ax; box->y = ay;
        box->width = cw; box->height = 0;
        float cursor = ay;
        for (NbBox *c = box->first_child; c; c = c->next_sibling) {
            float oh = do_layout(c, ax, cursor, cw, vw, vh);
            float cm = c->style ? (c->margin_top + c->margin_bottom) : 0;
            cursor += oh + cm;
        }
        box->height = cursor - ay;
        return box->height;
    }

    NbStyle *s  = box->style;
    float fs    = s->font_size > 0 ? s->font_size : 16.f;

    resolve_spacing(box, cw, vw, vh);

    /* content width */
    float inner = cw - box->margin_left - box->margin_right
                     - box->border_left - box->border_right
                     - box->padding_left - box->padding_right;
    if (inner < 0) inner = 0;
    if (s->width.unit != CSS_UNIT_AUTO) {
        float ew = rl(s->width, cw, fs, vw, vh);
        if (ew >= 0) inner = ew;
    }

    /* absolute position of content box */
    float cx = ax + box->margin_left + box->border_left + box->padding_left;
    float cy = ay + box->margin_top  + box->border_top  + box->padding_top;
    box->x     = cx;
    box->y     = cy;
    box->width = inner;

    /* ---- Flexbox ---- */
    if (box->type == BOX_FLEX) {
        int row = (s->flex_direction == FLEX_DIR_ROW ||
                   s->flex_direction == FLEX_DIR_ROW_REVERSE);
        float content_h = 0;
        if (row) {
            float fx = cx;
            float max_ch = 0;
            for (NbBox *c = box->first_child; c; c = c->next_sibling) {
                do_layout(c, fx, cy, inner, vw, vh);
                float ow = c->width  + c->border_left + c->border_right
                                     + c->padding_left + c->padding_right
                                     + c->margin_left  + c->margin_right;
                float oh = c->height + c->border_top  + c->border_bottom
                                     + c->padding_top  + c->padding_bottom;
                fx += ow;
                if (oh > max_ch) max_ch = oh;
            }
            content_h = max_ch;
        } else {
            float fy = cy;
            for (NbBox *c = box->first_child; c; c = c->next_sibling) {
                float oh = do_layout(c, cx, fy, inner, vw, vh);
                float cm = c->margin_top + c->margin_bottom;
                fy += oh + cm;
            }
            content_h = fy - cy;
        }
        if (s->height.unit != CSS_UNIT_AUTO) {
            float eh = rl(s->height, 0, fs, vw, vh);
            if (eh >= 0) content_h = eh;
        }
        box->height = fmaxf(0, content_h);
        return box->border_top + box->padding_top + box->height
             + box->padding_bottom + box->border_bottom;
    }

    /* ---- Inline context: all children are inline/text ---- */
    int has_block = 0;
    for (NbBox *c = box->first_child; c; c = c->next_sibling) {
        if (c->type == BOX_BLOCK || c->type == BOX_FLEX ||
            c->type == BOX_TABLE || c->type == BOX_TABLE_ROW ||
            c->type == BOX_TABLE_CELL) {
            has_block = 1; break;
        }
    }

    float content_h;
    if (!has_block && box->first_child) {
        content_h = layout_inline(box, cx, cy, inner, vw, vh);
    } else {
        /* ---- Block context ---- */
        float cursor = cy;
        for (NbBox *c = box->first_child; c; c = c->next_sibling) {
            float oh, cm;
            if (c->type == BOX_TEXT) {
                /* stray text node in block context */
                NbStyle *cs = c->style ? c->style : s;
                float tfs   = cs->font_size > 0 ? cs->font_size : fs;
                float lh    = cs->line_height > 0 ? cs->line_height : tfs * 1.4f;
                c->x = cx; c->y = cursor;
                c->width  = text_width_est(c->text, c->text_len, tfs);
                c->height = lh;
                oh = lh; cm = 0;
            } else {
                oh = do_layout(c, cx, cursor, inner, vw, vh);
                cm = c->margin_top + c->margin_bottom;
            }
            cursor += oh + cm;
        }
        content_h = cursor - cy;
    }

    if (s->height.unit != CSS_UNIT_AUTO) {
        float eh = rl(s->height, 0, fs, vw, vh);
        if (eh >= 0) content_h = eh;
    }
    box->height = fmaxf(0, content_h);

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

        /* Skip text inside non-visual parent tags */
        NbNode *par = node->parent;
        while (par) {
            if (par->type == NB_NODE_ELEMENT) {
                if (tag_is_invisible(par->tag)) return NULL;
                NbStyle *ps = (NbStyle*)par->computed_style;
                if (ps && ps->display == DISPLAY_NONE) return NULL;
                break;
            }
            par = par->parent;
        }

        /* Skip pure-whitespace */
        int all_ws = 1;
        for (const char *p = node->text; *p; p++)
            if ((unsigned char)*p > 32) { all_ws = 0; break; }
        if (all_ws) return NULL;

        NbBox *tb   = box_new(a, node, BOX_TEXT);
        tb->text     = node->text;
        tb->text_len = strlen(node->text);
        if (parent_box) tb->style = parent_box->style;
        return tb;
    }

    if (node->type != NB_NODE_ELEMENT) {
        /* Document node — just recurse */
        NbBox *root = box_new(a, node, BOX_BLOCK);
        for (NbNode *c = node->first_child; c; c = c->next_sibling) {
            NbBox *cb = build_box(a, c, root);
            if (cb) box_append(root, cb);
        }
        return root;
    }

    /* Element */
    if (tag_is_invisible(node->tag)) return NULL;

    NbStyle *st = (NbStyle*)node->computed_style;
    if (!st || st->display == DISPLAY_NONE) return NULL;

    NbBoxType btype;
    switch (st->display) {
        case DISPLAY_FLEX:            btype = BOX_FLEX;         break;
        case DISPLAY_INLINE_BLOCK:    btype = BOX_INLINE_BLOCK; break;
        case DISPLAY_TABLE:           btype = BOX_TABLE;        break;
        case DISPLAY_TABLE_ROW:       btype = BOX_TABLE_ROW;    break;
        case DISPLAY_TABLE_CELL:      btype = BOX_TABLE_CELL;   break;
        case DISPLAY_INLINE:          btype = BOX_INLINE;       break;
        default:                      btype = BOX_BLOCK;        break;
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

    layout->root->x = 0;
    layout->root->y = 0;
    do_layout(layout->root, 0, 0, vw, vw, vh);
    return layout;
}

void nb_layout_reflow(NbLayout *layout, float vw, float vh) {
    if (!layout || !layout->root) return;
    layout->viewport_width  = vw;
    layout->viewport_height = vh;
    layout->root->x = 0;
    layout->root->y = 0;
    do_layout(layout->root, 0, 0, vw, vw, vh);
}

void nb_layout_free(NbLayout *layout) {
    if (!layout) return;
    nb_arena_free(layout->arena);
    free(layout);
}

NbBox *nb_layout_box_at(NbLayout *layout, float x, float y) {
    if (!layout || !layout->root) return NULL;
    NbBox *best  = NULL;
    NbBox *stack[512];
    int    top   = 0;
    stack[top++] = layout->root;
    while (top > 0) {
        NbBox *b = stack[--top];
        float bx = b->x - b->border_left - b->padding_left;
        float by = b->y - b->border_top  - b->padding_top;
        float bw = b->width  + b->border_left + b->border_right
                             + b->padding_left + b->padding_right;
        float bh = b->height + b->border_top  + b->border_bottom
                             + b->padding_top  + b->padding_bottom;
        if (x >= bx && x <= bx+bw && y >= by && y <= by+bh) {
            best = b;
            for (NbBox *c = b->first_child; c; c = c->next_sibling)
                if (top < 511) stack[top++] = c;
        }
    }
    return best;
}
