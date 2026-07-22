#include "render.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */
static void set_color(cairo_t *cr, CssColor c) {
    cairo_set_source_rgba(cr,
        c.r / 255.0, c.g / 255.0, c.b / 255.0, c.a / 255.0);
}

static void fill_rect(cairo_t *cr, float x, float y, float w, float h, CssColor c) {
    if (c.is_transparent || c.a == 0 || w <= 0 || h <= 0) return;
    set_color(cr, c);
    cairo_rectangle(cr, x, y, w, h);
    cairo_fill(cr);
}

static void draw_border_side(cairo_t *cr, float x1, float y1, float x2, float y2,
                              float width, CssBorderStyle style, CssColor color) {
    if (style == BORDER_STYLE_NONE || width <= 0 || color.a == 0) return;
    set_color(cr, color);
    cairo_set_line_width(cr, width);
    if (style == BORDER_STYLE_DASHED) {
        double dashes[] = {6.0, 3.0}; cairo_set_dash(cr, dashes, 2, 0);
    } else if (style == BORDER_STYLE_DOTTED) {
        double dots[] = {2.0, 2.0}; cairo_set_dash(cr, dots, 2, 0);
    } else {
        cairo_set_dash(cr, NULL, 0, 0);
    }
    cairo_move_to(cr, x1, y1);
    cairo_line_to(cr, x2, y2);
    cairo_stroke(cr);
    cairo_set_dash(cr, NULL, 0, 0);
}

/* ------------------------------------------------------------------ */
/*  Text rendering via Pango                                           */
/* ------------------------------------------------------------------ */
static void draw_text(cairo_t *cr, float x, float y, float max_w,
                       const char *text, NbStyle *style) {
    if (!text || !text[0] || !style) return;

    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *desc = pango_font_description_new();

    /* font family */
    pango_font_description_set_family(desc, style->font_family);
    pango_font_description_set_size(desc, (int)(style->font_size * PANGO_SCALE));
    pango_font_description_set_weight(desc,
        style->font_weight >= 700 ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
    if (style->font_style == FONT_STYLE_ITALIC || style->font_style == FONT_STYLE_OBLIQUE)
        pango_font_description_set_style(desc, PANGO_STYLE_ITALIC);

    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);

    /* word wrap */
    if (max_w > 0) {
        pango_layout_set_width(layout, (int)(max_w * PANGO_SCALE));
        pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
    }

    pango_layout_set_text(layout, text, -1);

    /* text-align */
    if (style->text_align == TEXT_ALIGN_CENTER) pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
    else if (style->text_align == TEXT_ALIGN_RIGHT)  pango_layout_set_alignment(layout, PANGO_ALIGN_RIGHT);

    set_color(cr, style->color);
    cairo_move_to(cr, x, y);
    pango_cairo_show_layout(cr, layout);

    /* text decorations */
    if (style->text_decoration & TEXT_DECORATION_UNDERLINE) {
        int pw, ph; pango_layout_get_pixel_size(layout, &pw, &ph);
        cairo_set_line_width(cr, 1.0);
        set_color(cr, style->color);
        cairo_move_to(cr, x, y + ph - 2);
        cairo_line_to(cr, x + pw, y + ph - 2);
        cairo_stroke(cr);
    }
    if (style->text_decoration & TEXT_DECORATION_LINE_THROUGH) {
        int pw, ph; pango_layout_get_pixel_size(layout, &pw, &ph);
        cairo_set_line_width(cr, 1.0);
        set_color(cr, style->color);
        cairo_move_to(cr, x, y + ph/2);
        cairo_line_to(cr, x + pw, y + ph/2);
        cairo_stroke(cr);
    }

    g_object_unref(layout);
}

/* ------------------------------------------------------------------ */
/*  Box painting                                                       */
/* ------------------------------------------------------------------ */
static void paint_box(cairo_t *cr, NbBox *box, float ox, float oy) {
    if (!box) return;
    if (box->style && box->style->display == DISPLAY_NONE) return;
    if (box->style && box->style->visibility == VISIBILITY_HIDDEN) {
        /* still reserve space but don't paint */
        for (NbBox *c = box->first_child; c; c = c->next_sibling)
            paint_box(cr, c, ox, oy);
        return;
    }

    float bx = ox + box->x;
    float by = oy + box->y;
    float bw = box->width;
    float bh = box->height;

    /* ---- Background ---- */
    if (box->style) {
        /* Include padding in background */
        float bgx = bx - box->padding_left - box->border_left;
        float bgy = by - box->padding_top  - box->border_top;
        float bgw = bw + box->padding_left + box->padding_right + box->border_left + box->border_right;
        float bgh = bh + box->padding_top  + box->padding_bottom + box->border_top + box->border_bottom;
        fill_rect(cr, bgx, bgy, bgw, bgh, box->style->background_color);

        /* ---- Borders ---- */
        NbStyle *s = box->style;
        draw_border_side(cr, bgx, bgy, bgx+bgw, bgy,
                          s->border_top.width.value,    s->border_top.style,    s->border_top.color);
        draw_border_side(cr, bgx, bgy+bgh, bgx+bgw, bgy+bgh,
                          s->border_bottom.width.value, s->border_bottom.style, s->border_bottom.color);
        draw_border_side(cr, bgx, bgy, bgx, bgy+bgh,
                          s->border_left.width.value,   s->border_left.style,   s->border_left.color);
        draw_border_side(cr, bgx+bgw, bgy, bgx+bgw, bgy+bgh,
                          s->border_right.width.value,  s->border_right.style,  s->border_right.color);
    }

    /* ---- Text node ---- */
    if (box->type == BOX_TEXT && box->text && box->style) {
        draw_text(cr, bx, by, box->width, box->text, box->style);
        return; /* text nodes have no children */
    }

    /* ---- img element ---- */
    if (box->node && box->node->type == NODE_ELEMENT &&
        strcmp(box->node->tag, "img") == 0) {
        /* Draw placeholder box with alt text */
        CssColor border_clr = {180,180,180,255,0};
        CssColor bg_clr     = {240,240,240,255,0};
        fill_rect(cr, bx, by, bw > 0 ? bw : 100, bh > 0 ? bh : 60, bg_clr);
        const char *alt = nb_attr_val(box->node, "alt");
        if (alt && alt[0] && box->style) {
            NbStyle ts = *box->style;
            ts.color = (CssColor){100,100,100,255,0};
            draw_text(cr, bx+4, by+4, bw-8, alt, &ts);
        }
        /* draw dashed border */
        cairo_set_dash(cr, (double[]){4,4}, 2, 0);
        cairo_rectangle(cr, bx, by, bw > 0 ? bw : 100, bh > 0 ? bh : 60);
        cairo_set_source_rgba(cr, 0.7,0.7,0.7,1);
        cairo_set_line_width(cr, 1);
        cairo_stroke(cr);
        cairo_set_dash(cr, NULL, 0, 0);
    }

    /* ---- hr element ---- */
    if (box->node && box->node->type == NODE_ELEMENT &&
        strcmp(box->node->tag, "hr") == 0) {
        cairo_set_source_rgba(cr, 0.7,0.7,0.7,1);
        cairo_set_line_width(cr, 1);
        cairo_move_to(cr, bx, by + bh/2);
        cairo_line_to(cr, bx + bw, by + bh/2);
        cairo_stroke(cr);
    }

    /* ---- input/button element ---- */
    if (box->node && box->node->type == NODE_ELEMENT) {
        const char *tag = box->node->tag;
        if (strcmp(tag,"input")==0 || strcmp(tag,"textarea")==0 || strcmp(tag,"select")==0) {
            CssColor border = {160,160,160,255,0};
            CssColor bg     = {255,255,255,255,0};
            if (bw<=0) bw=200; if (bh<=0) bh=24;
            fill_rect(cr, bx, by, bw, bh, bg);
            cairo_set_source_rgba(cr, 0.6,0.6,0.6,1);
            cairo_set_line_width(cr, 1);
            cairo_rectangle(cr, bx, by, bw, bh);
            cairo_stroke(cr);
            const char *val = nb_attr_val(box->node,"value");
            if (val && box->style) {
                NbStyle ts = *box->style;
                ts.color=(CssColor){0,0,0,255,0};
                draw_text(cr, bx+4, by+4, bw-8, val, &ts);
            }
        }
        if (strcmp(tag,"button")==0) {
            CssColor border = {150,150,150,255,0};
            CssColor bg     = {230,230,230,255,0};
            if (bw<=0) bw=80; if (bh<=0) bh=28;
            fill_rect(cr, bx, by, bw, bh, bg);
            cairo_set_source_rgba(cr, 0.55,0.55,0.55,1);
            cairo_set_line_width(cr, 1);
            cairo_rectangle(cr, bx, by, bw, bh);
            cairo_stroke(cr);
        }
    }

    /* ---- Clip and recurse ---- */
    int do_clip = box->style &&
                  (box->style->overflow_x == OVERFLOW_HIDDEN ||
                   box->style->overflow_y == OVERFLOW_HIDDEN) &&
                  (bw > 0) && (bh > 0);
    if (do_clip) {
        cairo_save(cr);
        cairo_rectangle(cr, bx, by, bw, bh);
        cairo_clip(cr);
    }

    for (NbBox *c = box->first_child; c; c = c->next_sibling)
        paint_box(cr, c, ox, oy);

    if (do_clip) cairo_restore(cr);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */
void nb_render_paint(cairo_t *cr, NbLayout *layout, NbRenderState *state) {
    if (!cr || !layout) return;

    cairo_save(cr);
    cairo_translate(cr, -state->scroll_x, -state->scroll_y);
    if (state->scale != 1.0f && state->scale > 0)
        cairo_scale(cr, state->scale, state->scale);

    /* White page background */
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);

    paint_box(cr, layout->root, 0, 0);

    cairo_restore(cr);
}

void nb_render_selection(cairo_t *cr, float x, float y, float w, float h) {
    cairo_set_source_rgba(cr, 0.2, 0.5, 1.0, 0.3);
    cairo_rectangle(cr, x, y, w, h);
    cairo_fill(cr);
}

void nb_render_error_page(cairo_t *cr, float vw, float vh, const char *msg) {
    /* Light red background */
    cairo_set_source_rgb(cr, 0.98, 0.95, 0.95);
    cairo_paint(cr);

    /* Error header */
    PangoLayout *pl = pango_cairo_create_layout(cr);
    PangoFontDescription *fd = pango_font_description_from_string("Sans Bold 20");
    pango_layout_set_font_description(pl, fd);
    pango_font_description_free(fd);
    pango_layout_set_text(pl, "Nishant Browser — Could not load page", -1);
    cairo_set_source_rgb(cr, 0.7, 0.1, 0.1);
    cairo_move_to(cr, 40, 60);
    pango_cairo_show_layout(cr, pl);
    g_object_unref(pl);

    if (msg) {
        PangoLayout *ml = pango_cairo_create_layout(cr);
        PangoFontDescription *mfd = pango_font_description_from_string("Monospace 12");
        pango_layout_set_font_description(ml, mfd);
        pango_font_description_free(mfd);
        pango_layout_set_width(ml, (int)((vw - 80) * PANGO_SCALE));
        pango_layout_set_wrap(ml, PANGO_WRAP_WORD_CHAR);
        pango_layout_set_text(ml, msg, -1);
        cairo_set_source_rgb(cr, 0.3, 0.3, 0.3);
        cairo_move_to(cr, 40, 110);
        pango_cairo_show_layout(cr, ml);
        g_object_unref(ml);
    }
}

void nb_render_loading_page(cairo_t *cr, float vw, float vh, const char *url) {
    cairo_set_source_rgb(cr, 0.97, 0.97, 0.97);
    cairo_paint(cr);

    PangoLayout *pl = pango_cairo_create_layout(cr);
    PangoFontDescription *fd = pango_font_description_from_string("Sans 14");
    pango_layout_set_font_description(pl, fd);
    pango_font_description_free(fd);

    char buf[1024];
    snprintf(buf, sizeof(buf), "Loading  %s ...", url ? url : "");
    pango_layout_set_text(pl, buf, -1);

    cairo_set_source_rgb(cr, 0.4, 0.4, 0.4);
    int pw, ph; pango_layout_get_pixel_size(pl, &pw, &ph);
    cairo_move_to(cr, (vw - pw) / 2, (vh - ph) / 2);
    pango_cairo_show_layout(cr, pl);
    g_object_unref(pl);
}
