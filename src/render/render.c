#include "render.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

/* ====================================================================
 * render.c — Cairo + Pango paint layer
 *
 * Coordinate system: layout stores ABSOLUTE pixel positions in box->x/y.
 * paint_box uses box->x/y directly (no ox/oy accumulation).
 * scroll_x/scroll_y are applied once via cairo_translate in nb_render_paint.
 * ==================================================================== */

/* ------------------------------------------------------------------ */
/*  Colour helpers                                                     */
/* ------------------------------------------------------------------ */
static void set_color(cairo_t *cr, CssColor c) {
    if (c.is_transparent) { cairo_set_source_rgba(cr, 0,0,0,0); return; }
    cairo_set_source_rgba(cr,
        c.r / 255.0, c.g / 255.0, c.b / 255.0, c.a / 255.0);
}

static void fill_rect(cairo_t *cr, float x, float y, float w, float h, CssColor c) {
    if (c.is_transparent || c.a == 0 || w <= 0 || h <= 0) return;
    set_color(cr, c);
    cairo_rectangle(cr, x, y, w, h);
    cairo_fill(cr);
}

static void draw_border_side(cairo_t *cr,
                               float x1, float y1, float x2, float y2,
                               float width, CssBorderStyle style, CssColor color) {
    if (style == BORDER_STYLE_NONE || width <= 0 || color.a == 0) return;
    set_color(cr, color);
    cairo_set_line_width(cr, width);
    if (style == BORDER_STYLE_DASHED) {
        double d[] = {6,3}; cairo_set_dash(cr, d, 2, 0);
    } else if (style == BORDER_STYLE_DOTTED) {
        double d[] = {2,2}; cairo_set_dash(cr, d, 2, 0);
    } else {
        cairo_set_dash(cr, NULL, 0, 0);
    }
    cairo_move_to(cr, x1, y1);
    cairo_line_to(cr, x2, y2);
    cairo_stroke(cr);
    cairo_set_dash(cr, NULL, 0, 0);
}

/* ------------------------------------------------------------------ */
/*  Text rendering via Pango — uses absolute pixel font sizes         */
/* ------------------------------------------------------------------ */
static void draw_text_box(cairo_t *cr,
                           float x, float y, float max_w,
                           const char *text, NbStyle *style) {
    if (!text || !text[0] || !style) return;

    float font_size = style->font_size > 0 ? style->font_size : 16.0f;

    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *desc = pango_font_description_new();

    pango_font_description_set_family(desc,
        style->font_family[0] ? style->font_family : "Sans");
    /* Use absolute size (pixels → Pango units) */
    pango_font_description_set_absolute_size(desc,
        (int)(font_size * PANGO_SCALE));
    pango_font_description_set_weight(desc,
        style->font_weight >= 700 ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
    if (style->font_style == FONT_STYLE_ITALIC ||
        style->font_style == FONT_STYLE_OBLIQUE)
        pango_font_description_set_style(desc, PANGO_STYLE_ITALIC);

    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);

    if (max_w > 4) {
        pango_layout_set_width(layout, (int)(max_w * PANGO_SCALE));
        pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
    }
    pango_layout_set_text(layout, text, -1);

    if (style->text_align == TEXT_ALIGN_CENTER)
        pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
    else if (style->text_align == TEXT_ALIGN_RIGHT)
        pango_layout_set_alignment(layout, PANGO_ALIGN_RIGHT);

    set_color(cr, style->color);
    cairo_move_to(cr, x, y);
    pango_cairo_show_layout(cr, layout);

    /* Underline / strikethrough */
    if (style->text_decoration & (TEXT_DECORATION_UNDERLINE | TEXT_DECORATION_LINE_THROUGH)) {
        int pw, ph;
        pango_layout_get_pixel_size(layout, &pw, &ph);
        set_color(cr, style->color);
        cairo_set_line_width(cr, 1.0);
        if (style->text_decoration & TEXT_DECORATION_UNDERLINE) {
            cairo_move_to(cr, x, y + ph - 1);
            cairo_line_to(cr, x + pw, y + ph - 1);
            cairo_stroke(cr);
        }
        if (style->text_decoration & TEXT_DECORATION_LINE_THROUGH) {
            cairo_move_to(cr, x, y + ph/2);
            cairo_line_to(cr, x + pw, y + ph/2);
            cairo_stroke(cr);
        }
    }
    g_object_unref(layout);
}

/* ------------------------------------------------------------------ */
/*  Box painting — ABSOLUTE coordinates, no ox/oy accumulation        */
/* ------------------------------------------------------------------ */
static void paint_box(cairo_t *cr, NbBox *box) {
    if (!box) return;
    if (box->style && box->style->display == DISPLAY_NONE) return;
    if (box->style && box->style->visibility == VISIBILITY_HIDDEN) {
        for (NbBox *c = box->first_child; c; c = c->next_sibling)
            paint_box(cr, c);
        return;
    }

    /* Absolute content-box position */
    float bx = box->x;
    float by = box->y;
    float bw = box->width;
    float bh = box->height;

    /* ---- Background (extends into padding/border area) ---- */
    if (box->style && !box->style->background_color.is_transparent) {
        float bgx = bx - box->padding_left - box->border_left;
        float bgy = by - box->padding_top  - box->border_top;
        float bgw = bw + box->padding_left + box->padding_right
                       + box->border_left  + box->border_right;
        float bgh = bh + box->padding_top  + box->padding_bottom
                       + box->border_top   + box->border_bottom;
        if (bgw > 0 && bgh > 0)
            fill_rect(cr, bgx, bgy, bgw, bgh, box->style->background_color);
    }

    /* ---- Borders ---- */
    if (box->style) {
        NbStyle *s = box->style;
        float bgx = bx - box->padding_left - box->border_left;
        float bgy = by - box->padding_top  - box->border_top;
        float bgw = bw + box->padding_left + box->padding_right
                       + box->border_left  + box->border_right;
        float bgh = bh + box->padding_top  + box->padding_bottom
                       + box->border_top   + box->border_bottom;
        if (s->border_top.style != BORDER_STYLE_NONE)
            draw_border_side(cr, bgx, bgy, bgx+bgw, bgy,
                s->border_top.width.value, s->border_top.style, s->border_top.color);
        if (s->border_bottom.style != BORDER_STYLE_NONE)
            draw_border_side(cr, bgx, bgy+bgh, bgx+bgw, bgy+bgh,
                s->border_bottom.width.value, s->border_bottom.style, s->border_bottom.color);
        if (s->border_left.style != BORDER_STYLE_NONE)
            draw_border_side(cr, bgx, bgy, bgx, bgy+bgh,
                s->border_left.width.value, s->border_left.style, s->border_left.color);
        if (s->border_right.style != BORDER_STYLE_NONE)
            draw_border_side(cr, bgx+bgw, bgy, bgx+bgw, bgy+bgh,
                s->border_right.width.value, s->border_right.style, s->border_right.color);
    }

    /* ---- Text node ---- */
    if (box->type == BOX_TEXT && box->text && box->style) {
        draw_text_box(cr, bx, by, bw > 0 ? bw : 9999.0f, box->text, box->style);
        return;
    }

    /* ---- Special elements ---- */
    if (box->node && box->node->type == NB_NODE_ELEMENT) {
        const char *tag = box->node->tag;

        /* img — placeholder */
        if (strcmp(tag, "img") == 0) {
            float iw = bw > 4 ? bw : 100;
            float ih = bh > 4 ? bh : 60;
            CssColor bg = {230,230,230,255,0};
            fill_rect(cr, bx, by, iw, ih, bg);
            const char *alt = nb_attr_val(box->node, "alt");
            if (alt && alt[0] && box->style) {
                NbStyle ts = *box->style;
                ts.color = (CssColor){80,80,80,255,0};
                ts.font_size = fminf(ts.font_size, 12.0f);
                draw_text_box(cr, bx+3, by+3, iw-6, alt, &ts);
            }
            cairo_set_source_rgba(cr, 0.6,0.6,0.6,1);
            cairo_set_line_width(cr, 1);
            cairo_set_dash(cr, (double[]){4,4}, 2, 0);
            cairo_rectangle(cr, bx, by, iw, ih);
            cairo_stroke(cr);
            cairo_set_dash(cr, NULL, 0, 0);
            for (NbBox *c = box->first_child; c; c = c->next_sibling)
                paint_box(cr, c);
            return;
        }

        /* hr */
        if (strcmp(tag, "hr") == 0) {
            cairo_set_source_rgba(cr, 0.6,0.6,0.6,1);
            cairo_set_line_width(cr, 1);
            float mid = by + bh / 2;
            cairo_move_to(cr, bx, mid);
            cairo_line_to(cr, bx + bw, mid);
            cairo_stroke(cr);
            return;
        }

        /* input / textarea */
        if (strcmp(tag, "input") == 0 || strcmp(tag, "textarea") == 0) {
            float fw = bw > 4 ? bw : 180, fh = bh > 4 ? bh : 22;
            cairo_set_source_rgb(cr, 1,1,1);
            cairo_rectangle(cr, bx, by, fw, fh); cairo_fill(cr);
            cairo_set_source_rgba(cr, 0.5,0.5,0.5,1);
            cairo_set_line_width(cr, 1);
            cairo_rectangle(cr, bx, by, fw, fh); cairo_stroke(cr);
            const char *val = nb_attr_val(box->node, "value");
            if (val && val[0] && box->style) {
                NbStyle ts = *box->style;
                ts.color = (CssColor){0,0,0,255,0};
                draw_text_box(cr, bx+4, by+3, fw-8, val, &ts);
            }
            return;
        }

        /* button */
        if (strcmp(tag, "button") == 0) {
            float bw2 = bw > 4 ? bw : 80, bh2 = bh > 4 ? bh : 24;
            cairo_set_source_rgb(cr, 0.87,0.87,0.87);
            cairo_rectangle(cr, bx, by, bw2, bh2); cairo_fill(cr);
            cairo_set_source_rgba(cr, 0.4,0.4,0.4,1);
            cairo_set_line_width(cr, 1);
            cairo_rectangle(cr, bx, by, bw2, bh2); cairo_stroke(cr);
        }
    }

    /* ---- Overflow clip ---- */
    int do_clip = box->style &&
                  (box->style->overflow_x == OVERFLOW_HIDDEN ||
                   box->style->overflow_y == OVERFLOW_HIDDEN) &&
                  bw > 0 && bh > 0;
    if (do_clip) {
        cairo_save(cr);
        cairo_rectangle(cr, bx, by, bw, bh);
        cairo_clip(cr);
    }

    /* ---- Recurse into children ---- */
    for (NbBox *c = box->first_child; c; c = c->next_sibling)
        paint_box(cr, c);

    if (do_clip) cairo_restore(cr);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */
void nb_render_paint(cairo_t *cr, NbLayout *layout, NbRenderState *state) {
    if (!cr || !layout || !layout->root) return;

    cairo_save(cr);

    /* Apply scroll offset */
    float sx = state ? state->scroll_x : 0;
    float sy = state ? state->scroll_y : 0;
    float sc = (state && state->scale > 0) ? state->scale : 1.0f;

    if (sc != 1.0f) cairo_scale(cr, sc, sc);
    cairo_translate(cr, -sx / sc, -sy / sc);

    /* White page background */
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);

    paint_box(cr, layout->root);

    cairo_restore(cr);
}

void nb_render_selection(cairo_t *cr, float x, float y, float w, float h) {
    cairo_set_source_rgba(cr, 0.2, 0.5, 1.0, 0.3);
    cairo_rectangle(cr, x, y, w, h);
    cairo_fill(cr);
}

void nb_render_error_page(cairo_t *cr, float vw, float vh, const char *msg) {
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);

    /* Error header */
    PangoLayout *pl = pango_cairo_create_layout(cr);
    PangoFontDescription *fd = pango_font_description_from_string("Sans Bold 16px");
    pango_layout_set_font_description(pl, fd); pango_font_description_free(fd);
    pango_layout_set_text(pl, "Nishant Browser — Could not load page", -1);
    cairo_set_source_rgb(cr, 0.7, 0.1, 0.1);
    cairo_move_to(cr, 30, 40);
    pango_cairo_show_layout(cr, pl);
    g_object_unref(pl);

    if (msg && msg[0]) {
        PangoLayout *ml = pango_cairo_create_layout(cr);
        PangoFontDescription *mfd = pango_font_description_from_string("Monospace 12px");
        pango_layout_set_font_description(ml, mfd); pango_font_description_free(mfd);
        pango_layout_set_width(ml, (int)((vw - 60) * PANGO_SCALE));
        pango_layout_set_wrap(ml, PANGO_WRAP_WORD_CHAR);
        pango_layout_set_text(ml, msg, -1);
        cairo_set_source_rgb(cr, 0.3, 0.3, 0.3);
        cairo_move_to(cr, 30, 80);
        pango_cairo_show_layout(cr, ml);
        g_object_unref(ml);
    }
    (void)vh;
}

void nb_render_loading_page(cairo_t *cr, float vw, float vh, const char *url) {
    cairo_set_source_rgb(cr, 0.97, 0.97, 0.97);
    cairo_paint(cr);

    PangoLayout *pl = pango_cairo_create_layout(cr);
    PangoFontDescription *fd = pango_font_description_from_string("Sans 13px");
    pango_layout_set_font_description(pl, fd); pango_font_description_free(fd);

    char buf[1024];
    snprintf(buf, sizeof(buf), "Loading  %s ...", url ? url : "");
    pango_layout_set_text(pl, buf, -1);

    cairo_set_source_rgb(cr, 0.4, 0.4, 0.4);
    int pw, ph; pango_layout_get_pixel_size(pl, &pw, &ph);
    cairo_move_to(cr, (vw-pw)/2, (vh-ph)/2);
    pango_cairo_show_layout(cr, pl);
    g_object_unref(pl);
}
